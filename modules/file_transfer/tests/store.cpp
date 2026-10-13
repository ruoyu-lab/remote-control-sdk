// What the machine's half does with files, in a directory of the test's own:
// listing what to send (symbolic links listed, not followed), reading,
// writing into "<name>.<token>.rcsdk-part" (all of the token: transfers whose
// tokens share a beginning never meet) and renaming once whole (its time
// kept), names already there (each on_conflict), free space, links in the
// way, what a partial file's name holds that its transfer did not make, and
// the record that lets a later run go on (matched by token, path, size and
// time; anything else starts over; only for a transfer that resumes),
// discarded on request; a directory on an item's way swapped for a link
// between the store's check and its use, one being sent swapped while it is
// listed, and a source that became a pipe; answers that name no path of the
// machine; transfers going at once fitting the free space together.
#include "files.hpp"
#include "paths.hpp"
#include "record.hpp"
#include "store.hpp"
#include "support/check.hpp"

#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#include <winioctl.h>
#define getpid _getpid
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace rcsdk::file_transfer;
namespace fs = std::filesystem;

namespace {

constexpr int64_t someTime = 1'600'000'000'123'456'700; // Whole 100 ns: Windows keeps it as it is.

fs::path root;

std::string at(const fs::path& path) {
    return path.string();
}

void put(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
}

std::string contentOf(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// A symbolic link, where the system lets this process make one (Windows
// needs a privilege or developer mode).
bool link(const fs::path& target, const fs::path& at) {
    std::error_code error;
    fs::create_symlink(target, at, error);
    return !error;
}

// A link to a directory: a symbolic link, or on Windows a junction (which
// needs no privilege: any process that can write somewhere can make one).
bool redirect(const fs::path& target, const fs::path& at) {
#ifdef _WIN32
    if (!CreateDirectoryW(at.c_str(), nullptr)) {
        return false;
    }
    const HANDLE handle = CreateFileW(at.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    // A mount point's reparse data: the NT path, then the one to show, each NUL-terminated.
    const std::wstring shown = fs::absolute(target).wstring();
    const std::wstring nt = L"\\??\\" + shown;
    const auto bytes = [](const std::wstring& text) { return uint16_t(text.size() * sizeof(wchar_t)); };
    std::vector<uint16_t> data = {0, bytes(nt), uint16_t(bytes(nt) + sizeof(wchar_t)), bytes(shown)};
    data.insert(data.end(), nt.begin(), nt.end());
    data.push_back(0);
    data.insert(data.end(), shown.begin(), shown.end());
    data.push_back(0);
    std::vector<uint8_t> buffer(8 + data.size() * 2);
    const uint32_t tag = IO_REPARSE_TAG_MOUNT_POINT;
    const auto length = uint16_t(data.size() * 2);
    std::memcpy(buffer.data(), &tag, 4);
    std::memcpy(buffer.data() + 4, &length, 2);
    std::memcpy(buffer.data() + 8, data.data(), data.size() * 2);
    DWORD returned = 0;
    const BOOL set = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), DWORD(buffer.size()), nullptr,
                                     0, &returned, nullptr);
    CloseHandle(handle);
    return set != 0;
#else
    std::error_code error;
    fs::create_directory_symlink(target, at, error);
    return !error;
#endif
}

Token token(uint8_t seed) {
    Token t{};
    t.fill(seed);
    return t;
}

// Where the transfer of token `t` writes `path` until it is whole.
std::string part(const std::string& path, const Token& t = token(1)) {
    return path + "." + hex(t) + ".rcsdk-part";
}

template <class R>
R ask(Store& store, Request request) {
    auto reply = store.handle(std::move(request));
    CHECK(reply.has_value());
    return std::get<R>(*reply);
}

// What the store reads of item `i` of a listing (transfer `transfer`'s).
ReadRequest readOf(const Listed& listed, uint32_t i, uint64_t offset, uint32_t length, uint64_t transfer = 1) {
    const auto& item = listed.items[i];
    const auto& [entry, relative] = listed.sources[i];
    return {transfer, 1, i, item.path, listed.bases[entry], relative, item.size, item.modified, offset, length};
}

// The record of token `t` in `directory`.
fs::path recordIn(const fs::path& directory, const Token& t) {
    return directory / recordName(t);
}

Bytes bytes(const std::string& text) {
    return Bytes(text.begin(), text.end());
}

void listing() {
    const fs::path from = root / "send" / "photos";
    put(from / "b.txt", "bee");
    put(from / "a.txt", "ay");
    put(from / "sub" / "c.bin", std::string(1000, 'c'));
    fs::create_directories(from / "empty");
    const bool linked = link(from / "a.txt", from / "link");
    Store store;
    const auto listed = ask<Listed>(store, ListRequest{1, 100, {{at(from), ""}, {at(root / "send" / "photos" / "a.txt"), "renamed.txt"}}});
    CHECK(listed.result == RCS_OK);
    std::vector<std::string> paths;
    for (const auto& item : listed.items) {
        paths.push_back(item.path);
    }
    std::vector<std::string> expected = {"photos", "photos/a.txt", "photos/b.txt", "photos/empty"};
    if (linked) {
        expected.push_back("photos/link");
    }
    expected.insert(expected.end(), {"photos/sub", "photos/sub/c.bin", "renamed.txt"});
    CHECK(paths == expected);
    for (size_t i = 0; i < listed.items.size(); ++i) {
        const auto& item = listed.items[i];
        // Each file is read through its entry's directory (a file's own), then the names below it.
        const auto& [entry, relative] = listed.sources[i];
        if (item.path == "photos/sub/c.bin") {
            CHECK(item.kind == RCS_FILE_REGULAR && item.size == 1000 && entry == 0 && relative == "sub/c.bin");
        } else if (item.path == "renamed.txt") {
            CHECK(entry == 1 && relative == "a.txt");
        } else if (item.path == "photos/link") {
            CHECK(item.kind == RCS_FILE_OTHER && relative.empty()); // Not followed.
        } else if (item.path == "photos" || item.path == "photos/empty") {
            CHECK(item.kind == RCS_FILE_DIRECTORY && relative.empty());
        }
    }
    CHECK(listed.bases.size() == 2 && fs::path(listed.bases[0]) == from && fs::path(listed.bases[1]) == from);
    // What cannot go: a path not there, a relative one, a name not taken, too many.
    CHECK(ask<Listed>(store, ListRequest{2, 100, {{at(root / "nothing"), ""}}}).result == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(ask<Listed>(store, ListRequest{2, 100, {{"relative/path", ""}}}).result == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(ask<Listed>(store, ListRequest{2, 3, {{at(from), ""}}}).result == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(ask<Listed>(store, ListRequest{2, 100, {{at(from / "a.txt"), "x"}, {at(from / "b.txt"), "x"}}}).result ==
          RCS_ERROR_INVALID_ARGUMENT); // Two go as one name.
    put(root / "send" / "odd" / "x.rcsdk-part", "!");
    const auto odd = ask<Listed>(store, ListRequest{2, 100, {{at(root / "send" / "odd"), ""}}});
    CHECK(odd.result == RCS_ERROR_INVALID_ARGUMENT && odd.message.find("odd/x.rcsdk-part") != std::string::npos);

    // Reading: pieces as asked; a file that changed since it was listed fails.
    const auto data = ask<Data>(store, readOf(listed, 2, 1, 2));
    CHECK(data.data == bytes("ee") && data.offset == 1);
    put(from / "b.txt", "changed");
    const auto changed = ask<ReadFailed>(store, readOf(listed, 2, 0, 3));
    CHECK(changed.result == RCS_ERROR_INVALID_STATE);
    // A directory below the entry swapped for a link after it was listed:
    // the file is not read through it (another process may make one lead
    // anywhere).
    uint32_t c = 0;
    while (listed.items[c].path != "photos/sub/c.bin") {
        ++c;
    }
    fs::rename(from / "sub", root / "send" / "elsewhere");
    if (link(root / "send" / "elsewhere", from / "sub")) {
        const auto swapped = ask<ReadFailed>(store, readOf(listed, c, 0, 10, 2));
        CHECK(swapped.result == RCS_ERROR_INVALID_STATE);
    }
}

ItemSpec file(const std::string& path, const std::string& content) {
    return {RCS_FILE_REGULAR, content.size(), someTime, path};
}

// Writes `content` as the item (in pieces of 3 bytes) and finishes it.
Finished receive(Store& store, const fs::path& into, uint32_t item, const ItemSpec& spec, const std::string& content,
                 rcs_file_conflict conflict, const Token& t = token(1), bool keep = false) {
    for (size_t offset = 0; offset < content.size(); offset += 3) {
        const std::string piece = content.substr(offset, 3);
        const auto written =
            ask<Written>(store, WriteRequest{7, 1, item, at(into), spec, t, keep, offset == 0, offset, bytes(piece)});
        CHECK(written.offset == offset + piece.size());
        CHECK(fs::exists(fs::path(at(into)) / part(spec.path, t)));
    }
    return ask<Finished>(store, FinishRequest{7, 1, item, at(into), spec, conflict, t, keep});
}

void receiving() {
    const fs::path into = root / "in";
    fs::create_directories(into);
    Store store;
    // Temp file, then its name, with its time.
    const auto spec = file("docs/report.txt", "hello world");
    const auto done = receive(store, into, 0, spec, "hello world", RCS_FILE_CONFLICT_RENAME);
    CHECK(done.state == RCS_FILE_ITEM_COMPLETED && done.saved == "docs/report.txt");
    CHECK(contentOf(into / "docs" / "report.txt") == "hello world");
    CHECK(!fs::exists(into / part("docs/report.txt")));
    CHECK(files::status(at(into / "docs" / "report.txt"), "report")->modified == someTime);

    // Names taken, as on_conflict says.
    const std::string report = "docs/report.txt";
    const auto renamed = receive(store, into, 1, file(report, "second"), "second", RCS_FILE_CONFLICT_RENAME);
    CHECK(renamed.state == RCS_FILE_ITEM_COMPLETED && renamed.saved == "docs/report (1).txt");
    CHECK(contentOf(into / "docs" / "report (1).txt") == "second");
    const auto again = receive(store, into, 2, file(report, "third"), "third", RCS_FILE_CONFLICT_RENAME);
    CHECK(again.saved == "docs/report (2).txt");
    const auto over = receive(store, into, 3, file(report, "replaced"), "replaced", RCS_FILE_CONFLICT_OVERWRITE);
    CHECK(over.state == RCS_FILE_ITEM_COMPLETED && contentOf(into / "docs" / "report.txt") == "replaced");
    const auto skipped = receive(store, into, 4, file(report, "never"), "never", RCS_FILE_CONFLICT_SKIP);
    CHECK(skipped.state == RCS_FILE_ITEM_SKIPPED && skipped.result == RCS_OK);
    CHECK(contentOf(into / "docs" / "report.txt") == "replaced" && !fs::exists(into / part("docs/report.txt")));
    const auto failed = receive(store, into, 5, file(report, "never"), "never", RCS_FILE_CONFLICT_FAIL);
    CHECK(failed.state == RCS_FILE_ITEM_FAILED && failed.whole && failed.result == RCS_ERROR_INVALID_STATE);
    fs::create_directories(into / "folder");
    const auto onDirectory = receive(store, into, 6, file("folder", "x"), "x", RCS_FILE_CONFLICT_OVERWRITE);
    CHECK(onDirectory.state == RCS_FILE_ITEM_FAILED && fs::is_directory(into / "folder"));

    // An empty file, nothing written; directories, nested.
    const auto empty = ask<Finished>(store, FinishRequest{7, 1, 7, at(into), file("a/b/empty", ""),
                                                          RCS_FILE_CONFLICT_RENAME, token(1), false});
    CHECK(empty.state == RCS_FILE_ITEM_COMPLETED && fs::file_size(into / "a" / "b" / "empty") == 0);
    const auto made = ask<Finished>(store, FinishRequest{7, 1, 8, at(into), {RCS_FILE_DIRECTORY, 0, someTime, "x/y/z"},
                                                         RCS_FILE_CONFLICT_RENAME, token(1), false});
    CHECK(made.state == RCS_FILE_ITEM_COMPLETED && fs::is_directory(into / "x" / "y" / "z"));
    // The end: done when its answer comes.
    CHECK(ask<Ended>(store, CompleteRequest{7, at(into), token(1), {{"x", someTime}, {"x/y/z", someTime}}}).transfer ==
          7);
    CHECK(files::status(at(into / "x"), "x")->modified == someTime);

    // Bytes not all there, or out of order.
    ask<Written>(store, WriteRequest{7, 1, 9, at(into), file("short", "12345"), token(1), false, true, 0, bytes("12")});
    const auto partial = ask<Finished>(store, FinishRequest{7, 1, 9, at(into), file("short", "12345"),
                                                            RCS_FILE_CONFLICT_RENAME, token(1), false});
    CHECK(partial.state == RCS_FILE_ITEM_FAILED && !fs::exists(into / part("short")) && !fs::exists(into / "short"));
    const auto gap = ask<WriteFailed>(
        store, WriteRequest{7, 1, 10, at(into), file("gap", "123456"), token(1), false, true, 3, bytes("456")});
    CHECK(gap.result == RCS_ERROR_INVALID_STATE);

    // Free space, before anything is written.
    files::setFreeSpace([](const std::string&) { return std::optional<uint64_t>(10); });
    const auto full = ask<Prepared>(
        store, PrepareRequest{
                   8, at(into), RCS_FILE_CONFLICT_RENAME, token(2), false, false, {file("big", std::string(11, 'b'))}});
    CHECK(full.result == RCS_ERROR_DEVICE && full.message.find("11 bytes needed, 10 free") != std::string::npos);
    files::setFreeSpace({});
    CHECK(ask<Prepared>(
              store,
              PrepareRequest{
                  8, at(root / "missing"), RCS_FILE_CONFLICT_RENAME, token(2), false, false, {file("big", "b")}})
              .result == RCS_ERROR_INVALID_ARGUMENT);
    // Names taken, before it starts.
    const std::vector<ItemSpec> taken = {file("docs/report.txt", "x"), file("new.txt", "y"),
                                         {RCS_FILE_DIRECTORY, 0, 0, "docs/report (1).txt"}, file("folder", "z")};
    const auto skip =
        ask<Prepared>(store, PrepareRequest{8, at(into), RCS_FILE_CONFLICT_SKIP, token(2), false, false, taken});
    CHECK(skip.result == RCS_OK && skip.starts.size() == 3 && skip.starts[0].item == 0 &&
          skip.starts[0].what == Start::skip && skip.starts[1].item == 2 && skip.starts[1].what == Start::failed &&
          skip.starts[2].item == 3 && skip.starts[2].what == Start::skip);
    CHECK(ask<Prepared>(store, PrepareRequest{8, at(into), RCS_FILE_CONFLICT_FAIL, token(2), false, false, taken})
              .result == RCS_ERROR_INVALID_STATE);
    const auto overwrite =
        ask<Prepared>(store, PrepareRequest{8, at(into), RCS_FILE_CONFLICT_OVERWRITE, token(2), false, false, taken});
    CHECK(overwrite.result == RCS_OK && overwrite.starts.size() == 2 && overwrite.starts[1].item == 3 &&
          overwrite.starts[1].what == Start::failed);

    // Symbolic links are never followed: one in the way fails, one where the
    // partial file goes is replaced (what it points to stays as it was).
    const fs::path elsewhere = root / "elsewhere";
    put(elsewhere / "precious", "keep me");
    if (link(elsewhere, into / "linked")) {
        const auto blocked = ask<WriteFailed>(
            store, WriteRequest{7, 1, 11, at(into), file("linked/x", "abc"), token(1), false, true, 0, bytes("abc")});
        CHECK(blocked.result == RCS_ERROR_INVALID_STATE && !fs::exists(elsewhere / part("x")));
        CHECK(link(elsewhere / "precious", into / part("trap")));
        const auto trapped = ask<WriteFailed>(
            store, WriteRequest{7, 1, 12, at(into), file("trap", "new"), token(1), false, true, 0, bytes("new")});
        CHECK(trapped.result == RCS_ERROR_INVALID_STATE &&
              trapped.message.find("is there already") != std::string::npos);
        CHECK(contentOf(elsewhere / "precious") == "keep me" && fs::is_symlink(into / part("trap")));
        // Accepted as one that resumes (an earlier run of it may have left
        // what is there), the transfer starts the item over: what is in its
        // partial file's name goes (the link itself).
        CHECK(ask<Prepared>(
                  store,
                  PrepareRequest{7, at(into), RCS_FILE_CONFLICT_RENAME, token(1), true, false, {file("trap", "new")}})
                  .result == RCS_OK);
        const auto trap = receive(store, into, 12, file("trap", "new"), "new", RCS_FILE_CONFLICT_RENAME);
        CHECK(trap.state == RCS_FILE_ITEM_COMPLETED && contentOf(elsewhere / "precious") == "keep me" &&
              contentOf(into / "trap") == "new");
    } else {
        std::printf("no symbolic links here: their checks skipped\n");
    }

    // Discarded: the partial files go, an item's without an answer, the
    // end's with one; closing files at the end answers too.
    ask<Written>(store,
                 WriteRequest{9, 1, 0, at(into), file("gone", "abcdef"), token(3), false, true, 0, bytes("abc")});
    CHECK(!store.handle(DiscardRequest{9, at(into), token(3), false, {"gone"}}));
    CHECK(!fs::exists(into / part("gone", token(3))));
    ask<Written>(store,
                 WriteRequest{9, 1, 1, at(into), file("left", "abcdef"), token(3), false, true, 0, bytes("abc")});
    CHECK(ask<Ended>(store, DiscardRequest{9, at(into), token(3), true, {"left"}}).transfer == 9);
    CHECK(!fs::exists(into / part("left", token(3))));
    CHECK(ask<Ended>(store, CloseRequest{9}).transfer == 9);
}

// A transfer the connection ended (keep_partial), offered again by a later
// run with its token: whole files are done, a partial one goes on, a file
// that changed starts over; discarding deletes what was kept.
void resuming() {
    const fs::path into = root / "resume";
    fs::create_directories(into);
    const Token t = token(7);
    const auto whole = file("whole.txt", "all of it");
    const auto half = file("dir/half.bin", "0123456789");
    const auto stale = file("stale.txt", "old content");
    {
        Store first;
        CHECK(receive(first, into, 0, whole, "all of it", RCS_FILE_CONFLICT_RENAME, t, true).state ==
              RCS_FILE_ITEM_COMPLETED);
        ask<Written>(first, WriteRequest{7, 1, 1, at(into), half, t, true, true, 0, bytes("01234")});
        ask<Written>(first, WriteRequest{7, 1, 2, at(into), stale, t, true, true, 0, bytes("old")});
        first.shutdown(); // The run ends: kept.
    }
    CHECK(fs::exists(into / part("dir/half.bin", t)) && fs::exists(recordIn(into, t)));
    const auto kept = record::read(files::Directory::open(at(into), "in"), t);
    CHECK(kept.size() == 3 && kept.at("whole.txt").completed && kept.at("whole.txt").saved == "whole.txt" &&
          !kept.at("dir/half.bin").completed);
    CHECK(record::read(files::Directory::open(at(into), "in"), token(8)).empty()); // Another token: no record.

    Store second;
    auto changed = stale;
    changed.modified += 100; // It changed at the sending end since.
    const std::vector<ItemSpec> items = {whole, half, changed, file("fresh.txt", "new")};
    // An offer of that token that does not say it resumes (its host was not
    // asked about that) takes nothing of it, nor deletes it: its sender may
    // be another than the one that started it.
    const auto unsaid =
        ask<Prepared>(second, PrepareRequest{8, at(into), RCS_FILE_CONFLICT_RENAME, t, false, true, items});
    CHECK(unsaid.result == RCS_ERROR_INVALID_STATE && unsaid.message.find("does not resume") != std::string::npos);
    CHECK(contentOf(into / part("dir/half.bin", t)) == "01234" && contentOf(into / part("stale.txt", t)) == "old");
    const auto prepared =
        ask<Prepared>(second, PrepareRequest{8, at(into), RCS_FILE_CONFLICT_RENAME, t, true, true, items});
    CHECK(prepared.result == RCS_OK && prepared.starts.size() == 2);
    CHECK(prepared.starts[0].item == 0 && prepared.starts[0].what == Start::done &&
          prepared.starts[0].text == "whole.txt");
    CHECK(prepared.starts[1].item == 1 && prepared.starts[1].what == Start::resume && prepared.starts[1].offset == 5);
    // Another transfer into the same place does not take them.
    const auto other =
        ask<Prepared>(second, PrepareRequest{9, at(into), RCS_FILE_CONFLICT_RENAME, token(8), true, true, items});
    CHECK(other.result == RCS_OK && other.starts.empty());
    // It goes on: from 5 on, then whole.
    ask<Written>(second, WriteRequest{8, 1, 1, at(into), half, t, true, true, 5, bytes("56789")});
    const auto finished = ask<Finished>(second, FinishRequest{8, 1, 1, at(into), half, RCS_FILE_CONFLICT_RENAME, t, true});
    CHECK(finished.state == RCS_FILE_ITEM_COMPLETED && contentOf(into / "dir" / "half.bin") == "0123456789");
    // The changed one starts over: its partial file went as it was accepted.
    ask<Written>(second, WriteRequest{8, 1, 2, at(into), changed, t, true, true, 0, bytes("old content")});
    CHECK(contentOf(into / part("stale.txt", t)) == "old content");
    CHECK(ask<Ended>(second, CompleteRequest{8, at(into), t, {}}).transfer == 8);
    CHECK(!fs::exists(recordIn(into, t)));

    // Kept, then discarded (rcs_file_discard).
    {
        Store third;
        ask<Written>(
            third, WriteRequest{10, 1, 0, at(into), file("left.bin", "abcdef"), token(9), true, true, 0, bytes("abc")});
        third.shutdown();
    }
    CHECK(fs::exists(into / part("left.bin", token(9))));
    Store fourth;
    const auto purged = ask<Purged>(fourth, PurgeRequest{11, at(into), token(9)});
    CHECK(purged.result == RCS_OK && purged.files == 1 && !fs::exists(into / part("left.bin", token(9))) &&
          !fs::exists(recordIn(into, token(9))));
    const auto nothing = ask<Purged>(fourth, PurgeRequest{12, at(into), token(9)});
    CHECK(nothing.result == RCS_OK && nothing.files == 0);
    // keep_partial 0: the half's partial files go when it stops.
    {
        Store fifth;
        ask<Written>(fifth, WriteRequest{13, 1, 0, at(into), file("temp.bin", "abcdef"), token(10), false, true, 0,
                                         bytes("abc")});
        CHECK(fs::exists(into / part("temp.bin", token(10))));
        fifth.shutdown();
    }
    CHECK(!fs::exists(into / part("temp.bin", token(10))) && !fs::exists(recordIn(into, token(10))));
}

// Two transfers whose tokens differ only in their last byte (a host may
// choose its tokens so) write a file of the same name into one directory at
// once, their pieces taking turns: each writes a partial file of its own,
// and both arrive whole, the second renamed.
void sharedPrefix() {
    const fs::path into = root / "prefix";
    fs::create_directories(into);
    const Token a = token(5);
    Token b = token(5);
    b.back() = 6;
    const auto ofA = file("same.bin", "AAAAAA"), ofB = file("same.bin", "BBBBBB");
    Store store;
    ask<Written>(store, WriteRequest{20, 1, 0, at(into), ofA, a, false, true, 0, bytes("AAA")});
    ask<Written>(store, WriteRequest{21, 1, 0, at(into), ofB, b, false, true, 0, bytes("BBB")});
    ask<Written>(store, WriteRequest{20, 1, 0, at(into), ofA, a, false, false, 3, bytes("AAA")});
    const auto first =
        ask<Finished>(store, FinishRequest{20, 1, 0, at(into), ofA, RCS_FILE_CONFLICT_RENAME, a, false});
    CHECK(first.state == RCS_FILE_ITEM_COMPLETED && first.saved == "same.bin");
    CHECK(contentOf(into / "same.bin") == "AAAAAA");
    ask<Written>(store, WriteRequest{21, 1, 0, at(into), ofB, b, false, false, 3, bytes("BBB")});
    const auto second =
        ask<Finished>(store, FinishRequest{21, 1, 0, at(into), ofB, RCS_FILE_CONFLICT_RENAME, b, false});
    CHECK(second.state == RCS_FILE_ITEM_COMPLETED && second.saved == "same (1).bin");
    CHECK(contentOf(into / "same (1).bin") == "BBBBBB");
}

// What a partial file's name holds that its transfer did not make, or no
// longer holds as it left it, is never written: not a file someone else put
// there, not through another name of a file (a hard link), not into a pipe.
// The transfer's own goes on: written again from 0 by a machine's half that
// took over (not fresh), resumed, or started over.
void foreignPartials() {
    const fs::path into = root / "foreign";
    const fs::path elsewhere = root / "foreign-elsewhere";
    put(elsewhere / "precious", "keep me");
    fs::create_directories(into);
    const Token t = token(11);
    Store store;
    // A file in a new partial file's name.
    put(into / part("planted.bin", t), "theirs");
    const auto planted = ask<WriteFailed>(
        store, WriteRequest{30, 1, 0, at(into), file("planted.bin", "mine"), t, false, true, 0, bytes("mine")});
    CHECK(planted.result == RCS_ERROR_INVALID_STATE && contentOf(into / part("planted.bin", t)) == "theirs");
    // The same transfer again from 0, once its partial file is there: cut and written over.
    ask<Written>(store, WriteRequest{31, 1, 0, at(into), file("again.bin", "abcdef"), t, true, true, 0, bytes("abc")});
    store.shutdown();
    Store next;
    ask<Written>(next, WriteRequest{31, 2, 0, at(into), file("again.bin", "abcdef"), t, true, false, 0, bytes("ab")});
    CHECK(contentOf(into / part("again.bin", t)) == "ab");
    // Resumed through a hard link: the record says the transfer started the
    // item, but its partial file's name is another name of a file now.
    const auto ten = file("linked.bin", "0123456789");
    ask<Written>(next, WriteRequest{32, 1, 1, at(into), ten, t, true, true, 0, bytes("0123")});
    next.shutdown();
    fs::remove(into / part("linked.bin", t));
    std::error_code noLink;
    fs::create_hard_link(elsewhere / "precious", into / part("linked.bin", t), noLink);
    if (!noLink) {
        Store third;
        const auto prepared =
            ask<Prepared>(third, PrepareRequest{33, at(into), RCS_FILE_CONFLICT_RENAME, t, true, true, {ten}});
        CHECK(prepared.result == RCS_OK && prepared.starts.size() == 1 && prepared.starts[0].offset == 7);
        const auto linked =
            ask<WriteFailed>(third, WriteRequest{33, 1, 0, at(into), ten, t, true, true, 7, bytes("789")});
        CHECK(linked.result == RCS_ERROR_INVALID_STATE && linked.message.find("hard link") != std::string::npos);
        CHECK(contentOf(elsewhere / "precious") == "keep me");
    } else {
        std::printf("no hard links here: their check skipped\n");
    }
#ifndef _WIN32
    // A pipe where a partial file was: refused at once, never waited on.
    // (Android's /data takes no pipes.)
    fs::remove(into / part("linked.bin", t));
    if (mkfifo((into / part("linked.bin", t)).c_str(), 0600) != 0) {
        std::printf("no pipes here: their check skipped\n");
    } else {
        Store fourth;
        const auto piped =
            ask<WriteFailed>(fourth, WriteRequest{34, 1, 0, at(into), ten, t, true, false, 4, bytes("4")});
        CHECK(piped.result == RCS_ERROR_INVALID_STATE && piped.message.find("pipe") != std::string::npos);
        // Accepted again, the item starts over: the pipe goes.
        const auto over =
            ask<Prepared>(fourth, PrepareRequest{35, at(into), RCS_FILE_CONFLICT_RENAME, t, true, true, {ten}});
        CHECK(over.result == RCS_OK && over.starts.empty() &&
              !fs::exists(fs::symlink_status(into / part("linked.bin", t))));
    }
#endif
    // Longer than its item: the item starts over, its partial file goes as it is accepted.
    fs::remove(into / part("linked.bin", t));
    put(into / part("linked.bin", t), "0123456789and more");
    Store fifth;
    const auto longer =
        ask<Prepared>(fifth, PrepareRequest{36, at(into), RCS_FILE_CONFLICT_RENAME, t, true, true, {ten}});
    CHECK(longer.result == RCS_OK && longer.starts.empty() && !fs::exists(into / part("linked.bin", t)));
    CHECK(receive(fifth, into, 0, ten, "0123456789", RCS_FILE_CONFLICT_RENAME, t, true).state ==
          RCS_FILE_ITEM_COMPLETED);
    CHECK(contentOf(into / "linked.bin") == "0123456789");
}

// Files under `path`, however deep.
size_t filesUnder(const fs::path& path) {
    size_t count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(path)) {
        count += entry.is_regular_file() ? 1 : 0;
    }
    return count;
}

// Another process that can write into the receiving directory swaps a
// directory on an item's way for a link to a place outside it, after the
// store checked the item's directories and before it makes, writes or
// renames anything in them: nothing ever lands outside. On POSIX the store
// works in the directories it opened, which move with the swap (what it
// writes lands there, never behind the link), and the link fails the item
// at its next step; on Windows the directories are held open and cannot be
// swapped.
void swapped() {
    const fs::path into = root / "swap";
    const fs::path outside = root / "swap-outside";
    fs::create_directories(into);
    fs::create_directories(outside / "sub"); // Where the item's path leads behind the link.
    Store store;
    store.log = [](const std::string&) {};
    fs::path swap; // Armed: swapped at the next check.
    bool moved = false;
    store.checked = [&](const std::string&) {
        if (swap.empty()) {
            return;
        }
        std::error_code error;
        fs::rename(swap, fs::path(swap).concat("-moved"), error);
        moved = !error;
        if (moved) {
            CHECK(redirect(outside, swap));
        }
        swap.clear();
    };
#ifdef _WIN32
    const bool swappable = false;
#else
    const bool swappable = true;
#endif

    // A directory on the way, as the partial file is made: it lands in the
    // directory that moved, and the link fails the item where it is named.
    swap = into / "docs";
    const auto spec = file("docs/sub/a.txt", "abcdef");
    ask<Written>(store, WriteRequest{40, 1, 0, at(into), spec, token(1), false, true, 0, bytes("abc")});
    CHECK(moved == swappable && filesUnder(outside) == 0);
    ask<Written>(store, WriteRequest{40, 1, 0, at(into), spec, token(1), false, false, 3, bytes("def")});
    const auto named =
        ask<Finished>(store, FinishRequest{40, 1, 0, at(into), spec, RCS_FILE_CONFLICT_RENAME, token(1), false});
    CHECK(filesUnder(outside) == 0);
    if (moved) {
        CHECK(named.state == RCS_FILE_ITEM_FAILED && named.result == RCS_ERROR_INVALID_STATE);
        CHECK(contentOf(into / "docs-moved" / "sub" / part("a.txt")) == "abcdef");
    } else {
        CHECK(named.state == RCS_FILE_ITEM_COMPLETED && contentOf(into / "docs" / "sub" / "a.txt") == "abcdef");
    }

    // Between the check of a finished file's directories and its rename.
    const auto whole = file("whole/w.txt", "xyz");
    ask<Written>(store, WriteRequest{41, 1, 0, at(into), whole, token(1), false, true, 0, bytes("xyz")});
    swap = into / "whole";
    const auto renamed =
        ask<Finished>(store, FinishRequest{41, 1, 0, at(into), whole, RCS_FILE_CONFLICT_RENAME, token(1), false});
    CHECK(moved == swappable && filesUnder(outside) == 0);
    CHECK(renamed.state == RCS_FILE_ITEM_COMPLETED && contentOf(into / (moved ? "whole-moved" : "whole") / "w.txt") == "xyz");

    // An empty file, made and named at the end: in the directory that moved.
    swap = into / "empty";
    fs::create_directories(into / "empty");
    const auto empty = ask<Finished>(
        store, FinishRequest{42, 1, 0, at(into), file("empty/e.txt", ""), RCS_FILE_CONFLICT_RENAME, token(1), false});
    CHECK(moved == swappable && filesUnder(outside) == 0);
    CHECK(empty.state == RCS_FILE_ITEM_COMPLETED &&
          fs::exists(into / (moved ? "empty-moved" : "empty") / "e.txt"));
    store.checked = nullptr;
}

// A link to a directory in the way (a junction on Windows: no privilege
// makes it) fails the item, at its offer and at its first piece, and
// nothing is made or written behind it.
void linkedDirectory() {
    const fs::path into = root / "linked-directory";
    const fs::path outside = root / "linked-directory-outside";
    fs::create_directories(into);
    fs::create_directories(outside / "sub");
    CHECK(redirect(outside, into / "jx"));
    Store store;
    store.log = [](const std::string&) {}; // What it could not clean up behind the link.
    const auto offered = ask<Prepared>(
        store,
        PrepareRequest{43, at(into), RCS_FILE_CONFLICT_RENAME, token(1), false, false, {file("jx/sub/f.txt", "abc")}});
    CHECK(offered.result == RCS_OK && offered.starts.size() == 1 && offered.starts[0].what == Start::failed &&
          offered.starts[0].result == RCS_ERROR_INVALID_STATE);
    const auto written = ask<WriteFailed>(
        store, WriteRequest{43, 1, 0, at(into), file("jx/sub/f.txt", "abc"), token(1), false, true, 0, bytes("abc")});
    CHECK(written.result == RCS_ERROR_INVALID_STATE);
    const auto empty = ask<Finished>(store, FinishRequest{43, 1, 1, at(into), file("jx/sub/e.txt", ""),
                                                          RCS_FILE_CONFLICT_RENAME, token(1), false});
    CHECK(empty.state == RCS_FILE_ITEM_FAILED && empty.result == RCS_ERROR_INVALID_STATE);
    const auto directory = ask<Finished>(store, FinishRequest{43, 1, 2, at(into), {RCS_FILE_DIRECTORY, 0, 0, "jx/new"},
                                                              RCS_FILE_CONFLICT_RENAME, token(1), false});
    CHECK(directory.state == RCS_FILE_ITEM_FAILED && !fs::exists(outside / "new"));
    CHECK(filesUnder(outside) == 0 && !fs::exists(outside / "sub" / "f.txt"));
}

// A directory below one being sent swapped for a link (to a place the
// sending account can read) after it was looked at, before it is listed:
// nothing behind the link is listed (the names and sizes would go to the
// other end in the offer); it goes as what it is now, not followed.
void swappedWhileListed() {
    const fs::path from = root / "send-swap" / "top";
    const fs::path outside = root / "send-swap-outside";
    put(from / "sub" / "mine.txt", "mine");
    put(outside / "secret.txt", "not for the other end");
    Store store;
    bool moved = false;
    store.checked = [&](const std::string& path) {
        if (path == "top/sub" && !moved) {
            fs::rename(from / "sub", root / "send-swap" / "sub-moved");
            moved = redirect(outside, from / "sub");
        }
    };
    const auto listed = ask<Listed>(store, ListRequest{60, 100, {{at(from), ""}}});
    CHECK(moved && listed.result == RCS_OK);
    for (const auto& item : listed.items) {
        CHECK(item.path.find("secret") == std::string::npos);
        CHECK(item.path != "top/sub" || item.kind == RCS_FILE_OTHER);
    }
}

// SA-16: what the store answers names files by their paths as items go,
// never by the machine's: its messages go to the other end (an item's
// outcome, why a transfer ended or was refused) and to logs a host may pass
// on. Neither the directories the hosts gave (an account's name, how its
// disk is laid out) are in them, nor the name of a file sent as another.
void pathsStayHere() {
    const fs::path place = root / "secret-place";
    const fs::path into = root / "private-in";
    put(place / "local-name.txt", "abc");
    fs::create_directories(into);
    Store store;
    store.log = [](const std::string&) {};
    std::vector<std::string> messages;
    // Sending: a file sent as another name that changed, then went; a path
    // not there; a name below a directory that cannot go.
    const auto listed = ask<Listed>(store, ListRequest{50, 10, {{at(place / "local-name.txt"), "given.txt"}}});
    CHECK(listed.result == RCS_OK);
    put(place / "local-name.txt", "changed");
    messages.push_back(ask<ReadFailed>(store, readOf(listed, 0, 0, 3, 50)).message);
    CHECK(messages.back().find("given.txt") != std::string::npos);
    fs::remove(place / "local-name.txt");
    messages.push_back(ask<ReadFailed>(store, readOf(listed, 0, 0, 3, 51)).message);
    messages.push_back(ask<Listed>(store, ListRequest{52, 10, {{at(place / "gone"), "x"}}}).message);
    put(place / "dir" / "bad.rcsdk-part", "!");
    messages.push_back(ask<Listed>(store, ListRequest{53, 10, {{at(place / "dir"), "d"}}}).message);
    // Receiving: a directory not there, too little space, a name taken
    // (FAIL), a partial file gone, a file where a directory goes.
    const auto prepare = [&](uint64_t transfer, const fs::path& directory, rcs_file_conflict conflict,
                             const ItemSpec& item) {
        return ask<Prepared>(store, PrepareRequest{transfer, at(directory), conflict, token(20), false, false, {item}});
    };
    messages.push_back(prepare(54, root / "missing", RCS_FILE_CONFLICT_RENAME, file("a", "1")).message);
    files::setFreeSpace([](const std::string&) { return std::optional<uint64_t>(1); });
    messages.push_back(prepare(55, into, RCS_FILE_CONFLICT_RENAME, file("a", "12")).message);
    files::setFreeSpace({});
    put(into / "taken.txt", "x");
    messages.push_back(prepare(56, into, RCS_FILE_CONFLICT_FAIL, file("taken.txt", "1")).message);
    messages.push_back(ask<Finished>(store, FinishRequest{57, 1, 0, at(into), file("never.txt", "123"),
                                                          RCS_FILE_CONFLICT_RENAME, token(20), false})
                           .message);
    put(into / "plain", "x");
    messages.push_back(ask<WriteFailed>(store, WriteRequest{58, 1, 0, at(into), file("plain/x.txt", "abc"), token(20),
                                                            false, true, 0, bytes("abc")})
                           .message);
    messages.push_back(ask<Purged>(store, PurgeRequest{59, at(root / "missing"), token(20)}).message);
    for (const auto& message : messages) {
        const auto has = [&](const std::string& part) { return message.find(part) != std::string::npos; };
        CHECK(!message.empty() && !has(at(root)) && !has(root.filename().string()));
        CHECK(!has("secret-place") && !has("local-name") && !has("private-in"));
    }
}

// Transfers accepted at once into one volume: each is checked against its
// free space less what the others still have to write there, until they end.
void sharedSpace() {
    const fs::path into = root / "space";
    fs::create_directories(into);
    files::setFreeSpace([](const std::string&) { return std::optional<uint64_t>(10); });
    Store store;
    const auto prepare = [&](uint64_t transfer, const ItemSpec& item) {
        return ask<Prepared>(
            store, PrepareRequest{
                       transfer, at(into), RCS_FILE_CONFLICT_RENAME, token(uint8_t(transfer)), false, false, {item}});
    };
    CHECK(prepare(70, file("a", "123456")).result == RCS_OK);
    const auto both = prepare(71, file("b", "123456"));
    CHECK(both.result == RCS_ERROR_DEVICE &&
          both.message.find("6 bytes needed, 10 free, 6 of them for transfers going there") != std::string::npos);
    // What the first wrote it no longer needs; once it ends, it needs nothing.
    ask<Written>(store,
                 WriteRequest{70, 1, 0, at(into), file("a", "123456"), token(70), false, true, 0, bytes("1234")});
    CHECK(prepare(71, file("b", "123456")).result == RCS_OK);
    CHECK(prepare(72, file("c", "123")).result == RCS_ERROR_DEVICE); // 2 + 6 + 3.
    CHECK(ask<Ended>(store, CloseRequest{70}).transfer == 70);
    CHECK(prepare(72, file("c", "123")).result == RCS_OK);
    files::setFreeSpace({});
}

// A source that became a pipe after it was listed: refused at once, never
// waited on (nothing writes to it).
// SA-17 (Windows): a name opens what has it as its short (8.3) name too, so
// with OVERWRITE an item named by another file's short name would replace
// that file (its long name another than the host was shown). The item fails
// instead, and so does one whose directory is named by its short name; a file
// whose own long name has that form is replaced as any other.
void shortNames() {
#ifdef _WIN32
    const fs::path into = root / "short-names";
    put(into / "Quarterly report.txt", "keep");
    fs::create_directories(into / "Long directory name");
    const auto shortName = [](const fs::path& path) {
        wchar_t name[MAX_PATH];
        const DWORD size = GetShortPathNameW(path.c_str(), name, MAX_PATH);
        return size && size < MAX_PATH ? fs::path(std::wstring(name, size)).filename().string() : std::string();
    };
    const std::string aliasFile = shortName(into / "Quarterly report.txt");
    const std::string aliasDirectory = shortName(into / "Long directory name");
    if (aliasFile.empty() || aliasFile == "Quarterly report.txt" || aliasDirectory == "Long directory name") {
        std::printf("no short names on this volume (fsutil 8dot3name query): the short names' check skipped\n");
        return;
    }
    Store store;
    const auto replaced = receive(store, into, 0, file(aliasFile, "other"), "other", RCS_FILE_CONFLICT_OVERWRITE);
    CHECK(replaced.state == RCS_FILE_ITEM_FAILED && replaced.result == RCS_ERROR_INVALID_STATE);
    CHECK(replaced.message.find("short (8.3) name") != std::string::npos);
    CHECK(contentOf(into / "Quarterly report.txt") == "keep" && !fs::exists(into / part(aliasFile)));
    // RENAME gives it a free name beside the file instead.
    const auto renamed = receive(store, into, 1, file(aliasFile, "other"), "other", RCS_FILE_CONFLICT_RENAME);
    CHECK(renamed.state == RCS_FILE_ITEM_COMPLETED && renamed.saved != aliasFile);
    CHECK(contentOf(into / "Quarterly report.txt") == "keep");
    // A directory on the way.
    const auto inside = file(aliasDirectory + "/x.txt", "abc");
    const auto written =
        ask<WriteFailed>(store, WriteRequest{70, 1, 2, at(into), inside, token(1), false, true, 0, bytes("abc")});
    CHECK(written.result == RCS_ERROR_INVALID_STATE && written.message.find("short (8.3) name") != std::string::npos);
    CHECK(!fs::exists(into / "Long directory name" / "x.txt") &&
          !fs::exists(into / "Long directory name" / part("x.txt")));
    // A long name of that form is the file's own.
    put(into / "own" / "REPORT~1.TXT", "old");
    const auto own = receive(store, into, 3, file("own/REPORT~1.TXT", "new"), "new", RCS_FILE_CONFLICT_OVERWRITE);
    CHECK(own.state == RCS_FILE_ITEM_COMPLETED && contentOf(into / "own" / "REPORT~1.TXT") == "new");
#endif
}

void pipedSource() {
#ifndef _WIN32
    const fs::path from = root / "send-pipe";
    put(from / "f.txt", "data");
    Store store;
    const auto listed = ask<Listed>(store, ListRequest{3, 10, {{at(from / "f.txt"), ""}}});
    CHECK(listed.result == RCS_OK && listed.items.size() == 1);
    fs::remove(from / "f.txt");
    if (mkfifo((from / "f.txt").c_str(), 0600) != 0) {
        std::printf("no pipes here: the piped source's check skipped\n");
        return;
    }
    const auto& f = listed.items[0];
    const auto piped = ask<ReadFailed>(store, readOf(listed, 0, 0, 4, 3));
    CHECK(piped.result == RCS_ERROR_INVALID_STATE && piped.message.find("not a file") != std::string::npos);
#endif
}

} // namespace

int main() {
    root = fs::temp_directory_path() / ("rcsdk-test-file-transfer-store-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    const int result = runTest("file_transfer.store", [] {
        listing();
        receiving();
        resuming();
        sharedPrefix();
        foreignPartials();
        swapped();
        linkedDirectory();
        swappedWhileListed();
        pathsStayHere();
        sharedSpace();
        pipedSource();
        shortNames();
    });
    fs::remove_all(root);
    return result;
}
