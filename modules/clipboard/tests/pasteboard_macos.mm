// macOS: files on a pasteboard of the test's own (pasteboardWithUniqueName:
// never the general one, which the user and other programs use). Files
// copied are read by path, Finder's picture of their icons left out; files
// placed whole are file URLs; the other end's files are a stand-in whose
// provider gives Finder (as the test says who pastes) a hidden placeholder,
// sees where it is copied (FSEvents over a directory of the test's), asks
// for the files there and deletes the copy once they came; another program
// that pastes gets no file, and the paste is reported.
#include "fake_clipboard.hpp"
#include "stand_in_macos.hpp"
#include "support/check.hpp"

#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>

#include <copyfile.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>

using namespace rcsdk::clipboard;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

template <class Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

// What the stand-in asked the session's half for.
struct Asked {
    std::mutex mutex;
    std::vector<ToSession> messages;
    template <class T>
    std::optional<T> last() {
        std::lock_guard lock(mutex);
        for (auto at = messages.rbegin(); at != messages.rend(); ++at) {
            if (const auto* m = std::get_if<T>(&*at)) {
                return *m;
            }
        }
        return std::nullopt;
    }
};

std::vector<std::string> fileUrlsOn(NSPasteboard* board) {
    std::vector<std::string> paths;
    NSArray<NSURL*>* urls = [board readObjectsForClasses:@[ NSURL.class ]
                                                 options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    for (NSURL* url in urls) {
        paths.push_back(url.fileSystemRepresentation);
    }
    return paths;
}

void pasteboard() {
    const fs::path root = fs::temp_directory_path() / ("rcsdk-test-pasteboard-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "files" / "folder");
    fs::create_directories(root / "watched" / "Desktop");
    fs::create_directories(root / "own");
    std::ofstream(root / "files" / "a.txt") << "alpha";
    std::ofstream(root / "files" / "folder" / "b.txt") << "beta";
    const fs::path canonical = fs::canonical(root); // /private/var... as FSEvents says it.

    @autoreleasepool {
        NSPasteboard* board = [NSPasteboard pasteboardWithUniqueName];
        auto clipboard = createPasteboardClipboard((__bridge void*)board);
        Asked asked;
        auto pastes = std::make_shared<Pastes>([&](const ToSession& message) {
            std::lock_guard lock(asked.mutex);
            asked.messages.push_back(message);
        });
        pastes->configure((canonical / "own").string(), true);
        clipboard->usePastes(pastes);

        // Files a program copied (with Finder's icon picture beside them):
        // read by path, kind and size; the picture is no image copied.
        [board clearContents];
        [board writeObjects:@[
            [NSURL fileURLWithPath:@((root / "files" / "a.txt").c_str())],
            [NSURL fileURLWithPath:@((root / "files" / "folder").c_str())]
        ]];
        [board setData:[NSData dataWithBytes:"II*\0" length:4] forType:NSPasteboardTypeTIFF];
        Content read = clipboard->read(knownFormats, 1 << 20);
        CHECK(read.files && read.files->entries.size() == 2 && !(read.formats() & (1u << RCS_CLIPBOARD_IMAGE)));
        CHECK(read.files->entries[0].name == "a.txt" && read.files->entries[0].kind == RCS_FILE_REGULAR &&
              read.files->entries[0].size == 5 && read.files->entries[1].kind == RCS_FILE_DIRECTORY &&
              fs::path(read.files->entries[1].path) == fs::path(root / "files" / "folder"));
        CHECK(clipboard->read(allFormats, 1 << 20).leftOut & filesFormat); // Not asked for.
        CHECK(!clipboard->read(knownFormats, 30).files);                    // Too many bytes.

        // Files placed whole: file URLs, with text beside them.
        Content whole = test::filesCopy({read.files->entries[0]});
        whole.items.push_back(test::clipboardItem(RCS_CLIPBOARD_TEXT, "a.txt"));
        clipboard->write(whole);
        CHECK(fileUrlsOn(board) == std::vector<std::string>({fs::path(root / "files" / "a.txt").string()}));
        CHECK(std::string([board stringForType:NSPasteboardTypeString].UTF8String) == "a.txt");
        CHECK([board.types containsObject:@"org.nspasteboard.TransientType"]);

        // The other end's files: Finder pastes the placeholder; its copy is
        // seen where it went, the files asked for there, the copy deleted.
        setStandInSeams([] { return std::string("com.apple.finder"); }, {(canonical / "watched").string()});
        Content theirs = test::filesCopy({{"report.pdf", RCS_FILE_REGULAR, 10, 0, ""}});
        theirs.files->publication.fill(0xab);
        theirs.files->placing = 7;
        clipboard->write(theirs);
        const auto given = fileUrlsOn(board);
        CHECK(given.size() == 1 &&
              fs::path(given[0]).filename().string() == ".rcsdk-paste-abababababababababababababababab" &&
              fs::file_size(given[0]) == 0);
        const fs::path copied = canonical / "watched" / "Desktop" / fs::path(given[0]).filename();
        CHECK(copyfile(given[0].c_str(), copied.c_str(), nullptr, COPYFILE_ALL) == 0);
        CHECK(waitFor([&] { return asked.last<WantMessage>().has_value(); }));
        const auto want = *asked.last<WantMessage>();
        CHECK(want.placing == 7 && want.mode == RCS_FILE_FETCH_INTO &&
              fs::path(want.directory) == canonical / "watched" / "Desktop" && fs::exists(copied));
        pastes->ended(EndedMessage{want.request, RCS_OK, {}, true});
        CHECK(waitFor([&] { return !fs::exists(copied); }) && fs::exists(given[0])); // The placeholder stays for more.

        // Another program pastes: no file; the paste is reported.
        setStandInSeams([] { return std::string("com.apple.TextEdit"); }, {(canonical / "watched").string()});
        theirs.files->placing = 8;
        clipboard->write(theirs);
        CHECK(fileUrlsOn(board).empty());
        const auto refused = asked.last<RefusedMessage>();
        CHECK(refused && refused->placing == 8 && refused->message.find("com.apple.TextEdit") != std::string::npos);
        setStandInSeams({}, {});

        // An image goes as PNG with TIFF beside it (the item's own types, not
        // those the pasteboard translates to for a reader), but not one that
        // takes more than 256 MiB decoded (a small PNG of 9000 x 9000 the other
        // end sent): that goes as PNG alone, never decoded here.
        const auto png = [](size_t width, size_t height) {
            CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
            CGContextRef context = CGBitmapContextCreate(nullptr, width, height, 8, width, gray, kCGImageAlphaNone);
            CGImageRef picture = CGBitmapContextCreateImage(context);
            NSMutableData* out = [NSMutableData data];
            CGImageDestinationRef destination =
                CGImageDestinationCreateWithData((__bridge CFMutableDataRef)out, CFSTR("public.png"), 1, nullptr);
            CGImageDestinationAddImage(destination, picture, nullptr);
            CHECK(CGImageDestinationFinalize(destination));
            CFRelease(destination);
            CGImageRelease(picture);
            CGContextRelease(context);
            CGColorSpaceRelease(gray);
            return Bytes(static_cast<const uint8_t*>(out.bytes), static_cast<const uint8_t*>(out.bytes) + out.length);
        };
        Content image;
        image.items.push_back({RCS_CLIPBOARD_IMAGE, png(64, 32)});
        clipboard->write(image);
        NSArray<NSPasteboardType>* types = board.pasteboardItems.firstObject.types;
        CHECK([types containsObject:NSPasteboardTypePNG] && [types containsObject:NSPasteboardTypeTIFF]);
        image.items[0].bytes = png(9000, 9000);
        clipboard->write(image);
        types = board.pasteboardItems.firstObject.types;
        CHECK([types containsObject:NSPasteboardTypePNG] && ![types containsObject:NSPasteboardTypeTIFF]);
        clipboard.reset(); // Its stand-in's placeholder goes with it.
        [board releaseGlobally];
    }
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

} // namespace

int main() {
    return runTest("clipboard.pasteboard_macos", pasteboard);
}
