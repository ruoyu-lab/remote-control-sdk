// Clipboard copies: what fits, the wire form, and the watcher
// (on a clipboard of the test's). With RCSDK_TEST_CLIPBOARD=1, also the
// system's own clipboard: each format placed and read back, text's line
// endings, HTML's document, an image, a concealed copy. That replaces what the
// clipboard holds: run it where nobody works.
#include "content.hpp"
#include "fake_clipboard.hpp"
#include "messages.hpp"
#include "support/check.hpp"
#include "watcher.hpp"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace rcsdk::clipboard;
using namespace std::chrono_literals;

namespace {

template <class Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

bool protocolError(std::span<const uint8_t> wire, bool mayBeEmpty = false) {
    try {
        decode(wire, mayBeEmpty);
        return false;
    } catch (const Error& failure) {
        return failure.result() == RCS_ERROR_PROTOCOL;
    }
}

template <class Call>
bool throwsProtocol(Call call) {
    try {
        call();
        return false;
    } catch (const Error& failure) {
        return failure.result() == RCS_ERROR_PROTOCOL;
    }
}

void content() {
    Content copy = test::textCopy("text");
    copy.items.push_back(test::clipboardItem(RCS_CLIPBOARD_HTML, "<b>html</b>"));
    copy.items.push_back(test::pngItem(100));
    copy.concealed = true;
    CHECK(copy.formats() == ((1u << RCS_CLIPBOARD_TEXT) | (1u << RCS_CLIPBOARD_HTML) | (1u << RCS_CLIPBOARD_IMAGE)));
    CHECK(copy.bytes() == 4 + 11 + 100);
    // What fits, in format order; the rest is left out.
    const auto fitted = fitting(copy, allFormats, 20);
    CHECK(fitted.formats() == ((1u << RCS_CLIPBOARD_TEXT) | (1u << RCS_CLIPBOARD_HTML)) && fitted.concealed);
    CHECK(fitted.leftOut == (1u << RCS_CLIPBOARD_IMAGE));
    const auto textOnly = fitting(copy, 1u << RCS_CLIPBOARD_TEXT, 1000);
    CHECK(textOnly.items.size() == 1 && textOnly.leftOut == ((1u << RCS_CLIPBOARD_HTML) | (1u << RCS_CLIPBOARD_IMAGE)));
    CHECK(fitting(copy, allFormats, 3).items.empty());

    // The wire form round trip, and what it refuses.
    const Bytes wire = encode(copy);
    auto back = decode(wire);
    CHECK(back == copy);
    CHECK(protocolError(encode(Content{})));                       // Empty...
    CHECK(decode(encode(Content{}), true).items.empty()); // ...where it may be.
    auto cut = wire;
    cut.pop_back();
    CHECK(protocolError(cut));
    auto trailing = wire;
    trailing.push_back(0);
    CHECK(protocolError(trailing));
    auto flag = wire;
    flag[0] = 2;
    CHECK(protocolError(flag));
    Content twice = test::textCopy("a");
    twice.items.push_back(test::clipboardItem(RCS_CLIPBOARD_TEXT, "b"));
    CHECK(protocolError(encode(twice)));
    Content unordered;
    unordered.items = {test::pngItem(30), test::clipboardItem(RCS_CLIPBOARD_TEXT, "t")};
    CHECK(protocolError(encode(unordered)));
    CHECK(protocolError(encode(test::textCopy("\xC3")))); // Not UTF-8...
    CHECK(protocolError(encode(test::textCopy("\xC0\xAF"))));         // ...an overlong "/"...
    CHECK(protocolError(encode(test::textCopy("\xED\xA0\x80"))));     // ...a surrogate half...
    CHECK(protocolError(encode(test::textCopy("\xF4\x90\x80\x80")))); // ...past U+10FFFF...
    CHECK(!protocolError(encode(test::textCopy("\xF0\x9F\x98\x80\xE4\xB8\xAD")))); // ...but an emoji and 中 are.
    Content notPng, huge;
    notPng.items.push_back(test::clipboardItem(RCS_CLIPBOARD_IMAGE, std::string(30, 'x')));
    CHECK(protocolError(encode(notPng))); // An image the system's decoder never sees...
    auto big = test::pngItem(30);
    big.bytes[16] = 0x01; // ...a width of 16 777 217...
    huge.items.push_back(big);
    CHECK(protocolError(encode(huge)));
    Content rtf;
    rtf.items.push_back(test::clipboardItem(RCS_CLIPBOARD_RTF, "\xC3"));
    CHECK(!protocolError(encode(rtf))); // ...where only text must be.
    Content unknown;
    unknown.items.push_back(test::clipboardItem(5, "x"));
    CHECK(protocolError(encode(unknown)));
    Content hollow;
    hollow.items.push_back(test::clipboardItem(RCS_CLIPBOARD_TEXT, ""));
    CHECK(protocolError(encode(hollow)));
}

// A broker of pastes whose asks go nowhere.
std::shared_ptr<Pastes> quiet() {
    return std::make_shared<Pastes>([](const ToSession&) {});
}

Files someFiles() {
    Publication publication{};
    publication.fill(7);
    return Files{true,
                 publication,
                 {{"report.pdf", RCS_FILE_REGULAR, 123456, 1'700'000'000'000'000'000, "/Users/me/report.pdf"},
                  {"\xE7\x85\xA7\xE7\x89\x87", RCS_FILE_DIRECTORY, 0, -5, "/Users/me/\xE7\x85\xA7\xE7\x89\x87"}},
                 0};
}

// FILES: the wire form with and without paths, where it goes in what fits,
// what is refused; the messages that carry copies and pastes.
void files() {
    Content copy = test::textCopy("names");
    copy.files = someFiles();
    CHECK(copy.formats() == ((1u << RCS_CLIPBOARD_TEXT) | filesFormat));
    // Between the halves the paths travel; to the other end they do not.
    CHECK(decode(encode(copy, true), false, true) == copy);
    Content bare = decode(encode(copy));
    CHECK(bare.files && bare.files->entries[0].path.empty() && bare.files->entries[1].name == copy.files->entries[1].name &&
          bare.files->cut && bare.files->publication == copy.files->publication);
    CHECK(encode(copy).size() < encode(copy, true).size());
    // Files alone are a copy; FILES fits last, and counts its names.
    Content only;
    only.files = someFiles();
    CHECK(decode(encode(only)).files.has_value());
    const auto withoutFiles = fitting(copy, allFormats, 1 << 20);
    CHECK(!withoutFiles.files && withoutFiles.leftOut == filesFormat);
    const auto small = fitting(copy, knownFormats, 5 + 10);
    CHECK(small.items.size() == 1 && !small.files && (small.leftOut & filesFormat));
    CHECK(fitting(copy, knownFormats, 1 << 20).files == copy.files);
    CHECK(formatList(knownFormats) == "TEXT, HTML, RTF, IMAGE, FILES");
    // What is refused: names that are not one component, twice, kinds no
    // version defines, a directory with a size, none at all.
    for (const char* name : {"", ".", "..", "a/b", "a\\b", "tab\there"}) {
        Content wrong = only;
        wrong.files->entries[0].name = name;
        CHECK(protocolError(encode(wrong)));
    }
    // ...and any other name file transfer would not take (its path rules): a
    // copy's files are fetched, and placed where fetched ahead, by them (on
    // Windows "x:y" is a stream of x, "CON" a device, "x." is x).
    for (const char* name : {"x:y", "a*b", "q?", "<a>", "a\"b", "p|q", "x.", "x ", "CON", "nul.txt", "Com1", "lpt9.log",
                             "f.rcsdk-part", ".rcsdk-resume-0011"}) {
        Content wrong = only;
        wrong.files->entries[0].name = name;
        CHECK(protocolError(encode(wrong)));
        CHECK(!nameProblem(name).empty());
    }
    for (const char* name : {"Report 2024.txt", "console.log", "COM10", ".profile", "\xE4\xB8\xAD\xE6\x96\x87"}) {
        CHECK(nameProblem(name).empty());
    }
    // A copy's files fetched ahead: only files, of at most files_prefetch_bytes
    // together, whatever the copy said they were.
    CHECK(prefetchProblem(std::vector<FileItem>{{"a.txt", RCS_FILE_REGULAR, 600, 0}, {"b", RCS_FILE_REGULAR, 400, 0}},
                          1000)
              .empty());
    CHECK(!prefetchProblem(std::vector<FileItem>{{"a.txt", RCS_FILE_REGULAR, 600, 0}, {"b", RCS_FILE_REGULAR, 401, 0}},
                           1000)
               .empty());
    CHECK(!prefetchProblem(
               std::vector<FileItem>{{"a.txt", RCS_FILE_REGULAR, ~uint64_t(0), 0}, {"b", RCS_FILE_REGULAR, 2, 0}},
               1000)
               .empty()); // No sum that wraps.
    CHECK(
        !prefetchProblem(std::vector<FileItem>{{"d", RCS_FILE_DIRECTORY, 0, 0}, {"d/e", RCS_FILE_REGULAR, 1, 0}}, 1000)
             .empty());
    Content twice = only;
    twice.files->entries[1].name = twice.files->entries[0].name;
    twice.files->entries[1].kind = RCS_FILE_REGULAR;
    CHECK(protocolError(encode(twice)));
    Content other = only;
    other.files->entries[0].kind = RCS_FILE_OTHER;
    CHECK(protocolError(encode(other)));
    Content sized = only;
    sized.files->entries[1].size = 1;
    CHECK(protocolError(encode(sized)));
    Content none = only;
    none.files->entries.clear();
    CHECK(protocolError(encode(none)));
    auto cutFlag = encode(only);
    cutFlag[2] = 2; // Neither copied nor cut.
    CHECK(protocolError(cutFlag));
    // The hello takes FILES as a format.
    CHECK(decodeTakes(encodeTakes({knownFormats, 4096})) == (Takes{knownFormats, 4096}));

    // To the other end: a copy, and a publication withdrawn.
    CHECK(std::get<CopyMessage>(decodePeer(encode(PeerMessage{CopyMessage{bare}}))).content == bare);
    const auto withdrawn = std::get<WithdrawnMessage>(decodePeer(encode(PeerMessage{WithdrawnMessage{someFiles().publication}})));
    CHECK(withdrawn.publication == someFiles().publication);
    auto shortWithdrawn = encode(PeerMessage{WithdrawnMessage{}});
    shortWithdrawn.pop_back();
    CHECK(throwsProtocol([&] { decodePeer(shortWithdrawn); }));
    // Between the halves, each way.
    const Use use{true, 100, knownFormats, 4096, "/tmp/files", false};
    CHECK(std::get<UseMessage>(decodeToMachine(encode(ToMachine{UseMessage{use}}))).use == use);
    const auto place = std::get<PlaceMessage>(decodeToMachine(encode(ToMachine{PlaceMessage{9, bare, true}})));
    CHECK(place.id == 9 && place.ahead && place.content == bare);
    const ListedMessage listed{3, {{"photos", RCS_FILE_DIRECTORY, 0, 1}, {"photos/a.jpg", RCS_FILE_REGULAR, 10, 2}}};
    const auto listedBack = std::get<ListedMessage>(decodeToMachine(encode(ToMachine{listed})));
    CHECK(listedBack.request == 3 && listedBack.items == listed.items);
    const auto data = std::get<DataMessage>(decodeToMachine(encode(ToMachine{DataMessage{3, 8, RCS_OK, Bytes(100, 1)}})));
    CHECK(data.tag == 8 && data.data.size() == 100);
    CHECK(std::get<DataMessage>(decodeToMachine(encode(ToMachine{DataMessage{3, 8, RCS_ERROR_INVALID_STATE, {}}})))
              .result == RCS_ERROR_INVALID_STATE);
    CHECK(throwsProtocol([&] { decodeToMachine(encode(ToMachine{DataMessage{3, 8, RCS_ERROR_DEVICE, Bytes(1, 1)}})); }));
    const auto ended = std::get<EndedMessage>(decodeToMachine(encode(ToMachine{EndedMessage{3, RCS_OK, "", true}})));
    CHECK(ended.complete && ended.result == RCS_OK);
    CHECK(std::get<RevokedMessage>(decodeToMachine(encode(ToMachine{RevokedMessage{4}}))).placing == 4);
    const auto copied = std::get<CopiedMessage>(decodeToSession(encode(ToSession{CopiedMessage{false, copy}})));
    CHECK(copied.content.files == copy.files); // With its paths.
    const auto abilities =
        std::get<AbilitiesMessage>(decodeToSession(encode(ToSession{AbilitiesMessage{RCS_CLIPBOARD_SEND, "why"}})));
    CHECK(abilities.files == RCS_CLIPBOARD_SEND && abilities.why == "why");
    CHECK(throwsProtocol([&] { decodeToSession(encode(ToSession{AbilitiesMessage{4, ""}})); }));
    const auto want = std::get<WantMessage>(
        decodeToSession(encode(ToSession{WantMessage{5, 9, RCS_FILE_FETCH_INTO, "/Users/me/Desktop", false}})));
    CHECK(want.request == 5 && want.placing == 9 && want.directory == "/Users/me/Desktop");
    CHECK(throwsProtocol([&] { decodeToSession(encode(ToSession{WantMessage{5, 9, RCS_FILE_FETCH_INTO, "", false}})); }));
    CHECK(throwsProtocol([&] { decodeToSession(encode(ToSession{WantMessage{5, 9, 3, "", false}})); }));
    const auto read = std::get<ReadMessage>(decodeToSession(encode(ToSession{ReadMessage{5, 2, 1000, 4096, 77}})));
    CHECK(read.item == 2 && read.offset == 1000 && read.size == 4096 && read.tag == 77);
    CHECK(std::get<CloseMessage>(decodeToSession(encode(ToSession{CloseMessage{5, RCS_OK}}))).outcome == RCS_OK);
    const auto refused = std::get<RefusedMessage>(
        decodeToSession(encode(ToSession{RefusedMessage{9, RCS_ERROR_UNSUPPORTED, "TextEdit pastes"}})));
    CHECK(refused.placing == 9 && refused.message == "TextEdit pastes");
}

struct Watched {
    std::mutex mutex;
    std::vector<Content> copies;
    std::vector<bool> existing;
    std::vector<std::pair<rcs_result, std::string>> failures;
    std::vector<std::pair<uint64_t, rcs_result>> placed;

    Watcher::Handlers handlers() {
        Watcher::Handlers h;
        h.copied = [this](Content c, bool held) {
            std::lock_guard lock(mutex);
            copies.push_back(std::move(c));
            existing.push_back(held);
        };
        h.readFailed = [this](rcs_result result, const std::string& message) {
            std::lock_guard lock(mutex);
            failures.emplace_back(result, message);
        };
        h.placed = [this](uint64_t id, rcs_result result, const std::string&) {
            std::lock_guard lock(mutex);
            placed.emplace_back(id, result);
        };
        return h;
    }
    size_t copyCount() {
        std::lock_guard lock(mutex);
        return copies.size();
    }
    Content last() {
        std::lock_guard lock(mutex);
        return copies.empty() ? Content{} : copies.back();
    }
};

void watcher() {
    test::FakeClipboard board;
    setFactory([&](rcs_role role) {
        CHECK(role == RCS_ROLE_VIEW);
        return std::make_unique<test::FakeClipboardAccess>(board);
    });
    board.copy(test::textCopy("there first"));
    Watched watched;
    {
        Watcher watcher(RCS_ROLE_VIEW, {true, 20, allFormats, 1024}, watched.handlers(), quiet());
        // What it holds at the start; each new copy; nothing for no change.
        CHECK(waitFor([&] { return watched.copyCount() == 1 && watched.last() == test::textCopy("there first"); }));
        board.copy(test::textCopy("second"));
        CHECK(waitFor([&] { return watched.copyCount() == 2; }));
        {
            std::lock_guard lock(watched.mutex);
            CHECK(watched.existing == std::vector<bool>({true, false})); // What it held, then a new copy.
        }
        std::this_thread::sleep_for(100ms);
        CHECK(watched.copyCount() == 2);
        // What it places is no new copy, also when the settings change.
        watcher.place(5, test::textCopy("placed here"));
        CHECK(waitFor([&] {
            std::lock_guard lock(watched.mutex);
            return watched.placed.size() == 1 && watched.placed[0] == std::pair<uint64_t, rcs_result>(5, RCS_OK);
        }));
        CHECK(board.text() == "placed here");
        watcher.use({true, 20, 1u << RCS_CLIPBOARD_TEXT, 2048});
        std::this_thread::sleep_for(100ms);
        CHECK(watched.copyCount() == 2);
        // A copy of the user's is read afresh in what is taken now.
        board.copy(test::textCopy(std::string(3000, 'x')));
        CHECK(waitFor([&] { return watched.copyCount() == 3; }));
        CHECK(watched.last().items.empty() && watched.last().leftOut == (1u << RCS_CLIPBOARD_TEXT)); // Too large.
        watcher.use({true, 20, allFormats, 4096});
        CHECK(waitFor([&] { return watched.copyCount() == 4 && watched.last().items.size() == 1; }));
        // A clipboard another program holds: looked at again, then reported.
        board.busyReads = 2;
        board.copy(test::textCopy("after a moment"));
        CHECK(waitFor([&] { return watched.copyCount() == 5 && watched.last() == test::textCopy("after a moment"); }));
        board.busyReads = 100;
        board.copy(test::textCopy("never read"));
        CHECK(waitFor([&] {
            std::lock_guard lock(watched.mutex);
            return watched.failures.size() == 1 && watched.failures[0].first == RCS_ERROR_DEVICE;
        }));
        board.busyReads = 0;
        // Not watched: copies only placed. Watched again: what it holds is
        // reported as held, also when it did not change.
        watcher.use({false, 20, allFormats, 4096});
        board.copy(test::textCopy("unwatched"));
        std::this_thread::sleep_for(100ms);
        CHECK(watched.copyCount() == 5);
        watcher.use({true, 20, allFormats, 4096});
        CHECK(waitFor([&] {
            std::lock_guard lock(watched.mutex);
            return watched.copies.size() == 6 && watched.existing.back();
        }));
    }
    // No clipboard for this process: said once, and each copy to place fails.
    setFactory(
        [](rcs_role) -> std::unique_ptr<SystemClipboard> { fail(RCS_ERROR_UNSUPPORTED, "No clipboard here"); });
    Watched none;
    {
        Watcher watcher(RCS_ROLE_VIEW, {true, 20, allFormats, 1024}, none.handlers(), quiet());
        watcher.place(1, test::textCopy("x"));
        CHECK(waitFor([&] {
            std::lock_guard lock(none.mutex);
            return none.failures.size() == 1 && none.failures[0].first == RCS_ERROR_UNSUPPORTED &&
                   none.placed.size() == 1 && none.placed[0].second == RCS_ERROR_UNSUPPORTED;
        }));
    }
    setFactory({});
}

// ---- The system's clipboard ----------------------------------------------------------------

std::string textOf(const Content& content, rcs_clipboard_format format) {
    for (const auto& item : content.items) {
        if (item.format == format) {
            return std::string(item.bytes.begin(), item.bytes.end());
        }
    }
    return {};
}

// A 2x1 PNG: a red pixel and a blue one.
const std::vector<uint8_t> twoPixels = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x7B, 0x40, 0xE8, 0xDD, 0x00, 0x00, 0x00,
    0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0x00, 0x04, 0xFF, 0x01, 0x07, 0x00, 0x01, 0xFF,
    0xE2, 0x23, 0x9E, 0x59, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

void systemClipboard() {
    std::unique_ptr<SystemClipboard> clipboard;
    try {
        clipboard = createPlatformClipboard();
    } catch (const Error& failure) {
        std::fprintf(stderr, "No system clipboard here: %s\n", failure.what());
        CHECK(failure.result() == RCS_ERROR_UNSUPPORTED);
        return;
    }
    // Text of any script, with LF line endings both ways (Windows keeps CRLF
    // of its own).
    const std::string text = "first line\nsecond: \xE4\xB8\xAD\xE6\x96\x87 \xF0\x9F\x98\x80\n";
    const uint64_t before = clipboard->changeCount();
    const uint64_t after = clipboard->write(test::textCopy(text));
    CHECK(after != before && clipboard->changeCount() == after);
    auto read = clipboard->read(allFormats, 1 << 20);
    CHECK(textOf(read, RCS_CLIPBOARD_TEXT) == text && !read.concealed);
    // Every format at once, then within a size and in fewer formats.
    Content all = test::textCopy("plain");
    all.items.push_back(test::clipboardItem(RCS_CLIPBOARD_HTML, "<html><body><p>rich <b>text</b></p></body></html>"));
    all.items.push_back(test::clipboardItem(RCS_CLIPBOARD_RTF, "{\\rtf1\\ansi rich {\\b text}}"));
    all.items.push_back({RCS_CLIPBOARD_IMAGE, Bytes(twoPixels.begin(), twoPixels.end())});
    clipboard->write(all);
    read = clipboard->read(allFormats, 1 << 20);
    CHECK(textOf(read, RCS_CLIPBOARD_TEXT) == "plain");
    CHECK(textOf(read, RCS_CLIPBOARD_HTML).find("rich <b>text</b>") != std::string::npos);
    CHECK(textOf(read, RCS_CLIPBOARD_RTF).find("{\\rtf1") == 0);
    const std::string png = textOf(read, RCS_CLIPBOARD_IMAGE);
    CHECK(png.size() > 8 && png.compare(1, 3, "PNG") == 0);
    std::fprintf(stderr, "system clipboard: formats 0x%X, image %zu bytes\n", read.formats(), png.size());
    read = clipboard->read(1u << RCS_CLIPBOARD_TEXT, 1 << 20);
    CHECK(read.items.size() == 1 && (read.leftOut & (1u << RCS_CLIPBOARD_IMAGE)));
    read = clipboard->read(allFormats, 5);
    CHECK(read.items.size() == 1 && textOf(read, RCS_CLIPBOARD_TEXT) == "plain");
    // A concealed copy is marked so, and read back as concealed.
    clipboard->write(test::textCopy("a password", true));
    read = clipboard->read(allFormats, 1 << 20);
    CHECK(read.concealed && textOf(read, RCS_CLIPBOARD_TEXT) == "a password");
    clipboard->write(test::textCopy("rcsdk clipboard test done"));
}

} // namespace

int main() {
    int result = runTest("content", content);
    result |= runTest("files", files);
    result |= runTest("watcher", watcher);
    if (const char* value = std::getenv("RCSDK_TEST_CLIPBOARD"); value && std::string(value) == "1") {
        result |= runTest("system clipboard", systemClipboard);
    }
    return result;
}
