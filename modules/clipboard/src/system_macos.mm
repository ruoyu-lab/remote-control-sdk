// The system clipboard (SystemClipboard) on macOS: the general
// pasteboard. Text is public.utf8-plain-text, HTML public.html, RTF
// public.rtf; an image public.png, else public.tiff (converted with ImageIO),
// and it is placed as PNG and TIFF. A copy is concealed where its program
// marked it with the nspasteboard.org types (ConcealedType, TransientType:
// password managers do), and placed with ConcealedType. NSPasteboard is used
// from the caller's thread (it is not bound to the main one).
//
// Files (FILES) are the file URLs copied (Finder's), read by path; Finder's
// picture of their icons beside them is not an image copied, and is left
// out; promised files (another program's, written when pasted) are not read.
// Files placed whole are file URLs; the other end's go as a stand-in
// (stand_in_macos.mm). Either is placed on this Mac only (not through
// Universal Clipboard) and marked transient (clipboard managers keep none).
#include "stand_in_macos.hpp"

#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>

#include <sys/stat.h>

namespace rcsdk::clipboard {
namespace {

NSString* const concealedType = @"org.nspasteboard.ConcealedType";
NSString* const transientType = @"org.nspasteboard.TransientType";
NSString* const promisedType = @"com.apple.pasteboard.promised-file-url";

Bytes bytesOf(NSData* data) {
    if (!data) {
        return {};
    }
    const auto* begin = static_cast<const uint8_t*>(data.bytes);
    return Bytes(begin, begin + data.length);
}

Bytes utf8Of(NSString* text) {
    if (!text) {
        return {};
    }
    const char* utf8 = text.UTF8String;
    return utf8 ? Bytes(utf8, utf8 + std::strlen(utf8)) : Bytes{};
}

// An image's bytes as another type ("public.png", "public.tiff"); nil where
// ImageIO cannot read them, and for a picture that takes more than 256 MiB
// decoded (64 million pixels, as Windows' bitmap beside a PNG): a small
// file, e.g. a PNG the other end sent, may say any size up to 16384 x 16384.
NSData* converted(NSData* image, CFStringRef type) {
    constexpr uint64_t mostPixels = (uint64_t(256) << 20) / 4;
    CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)image, nullptr);
    if (!source) {
        return nil;
    }
    NSDictionary* properties = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr));
    const uint64_t width = [properties[(__bridge NSString*)kCGImagePropertyPixelWidth] unsignedLongLongValue];
    const uint64_t height = [properties[(__bridge NSString*)kCGImagePropertyPixelHeight] unsignedLongLongValue];
    if (!width || !height || width > mostPixels / height) {
        CFRelease(source);
        return nil;
    }
    CGImageRef picture = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    CFRelease(source);
    if (!picture) {
        return nil;
    }
    NSMutableData* out = [NSMutableData data];
    CGImageDestinationRef destination =
        CGImageDestinationCreateWithData((__bridge CFMutableDataRef)out, type, 1, nullptr);
    bool done = false;
    if (destination) {
        CGImageDestinationAddImage(destination, picture, nullptr);
        done = CGImageDestinationFinalize(destination);
        CFRelease(destination);
    }
    CGImageRelease(picture);
    return done ? out : nil;
}

// The files the pasteboard's file URLs name, as they are now (a link, or
// what is gone, is left out); nullopt without any.
std::optional<Files> filesOf(NSPasteboard* board) {
    NSArray<NSURL*>* urls = [board readObjectsForClasses:@[ NSURL.class ]
                                                 options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    if (!urls.count) {
        return std::nullopt;
    }
    Files files;
    for (NSURL* url in urls) {
        const char* path = url.fileSystemRepresentation;
        struct stat s {};
        if (!path || lstat(path, &s) != 0 || !(S_ISREG(s.st_mode) || S_ISDIR(s.st_mode))) {
            continue;
        }
        FileEntry entry;
        entry.path = path;
        entry.name = entry.path.substr(entry.path.rfind('/') + 1);
        entry.kind = S_ISDIR(s.st_mode) ? RCS_FILE_DIRECTORY : RCS_FILE_REGULAR;
        entry.size = S_ISREG(s.st_mode) ? uint64_t(s.st_size) : 0;
        entry.modified = int64_t(s.st_mtimespec.tv_sec) * 1'000'000'000 + s.st_mtimespec.tv_nsec;
        files.entries.push_back(std::move(entry));
    }
    return files.entries.empty() ? std::nullopt : std::optional(std::move(files));
}

class MacClipboard final : public SystemClipboard {
  public:
    explicit MacClipboard(NSPasteboard* board) : board_(board) {}

    uint64_t changeCount() override {
        @autoreleasepool {
            return uint64_t(board_.changeCount);
        }
    }

    Content read(uint32_t formats, size_t maxBytes) override {
        @autoreleasepool {
            NSPasteboard* board = board_;
            NSArray<NSPasteboardType>* types = board.types ? board.types : @[];
            Content content;
            content.concealed = [types containsObject:concealedType] || [types containsObject:transientType];
            size_t left = maxBytes;
            const auto wanted = [&](rcs_clipboard_format format, bool available) {
                if (available && !(formats & (1u << format))) {
                    content.leftOut |= 1u << format;
                }
                return available && (formats & (1u << format));
            };
            const auto add = [&](rcs_clipboard_format format, Bytes bytes) {
                if (bytes.empty()) {
                    return;
                }
                if (bytes.size() > left) {
                    content.leftOut |= 1u << format;
                    return;
                }
                left -= bytes.size();
                content.items.push_back({format, std::move(bytes)});
            };
            const bool fileUrls = [types containsObject:NSPasteboardTypeFileURL];
            const bool promised = [types containsObject:promisedType];
            if (wanted(RCS_CLIPBOARD_TEXT, [types containsObject:NSPasteboardTypeString])) {
                add(RCS_CLIPBOARD_TEXT, utf8Of([board stringForType:NSPasteboardTypeString]));
            }
            if (wanted(RCS_CLIPBOARD_HTML, [types containsObject:NSPasteboardTypeHTML])) {
                add(RCS_CLIPBOARD_HTML, utf8Of([board stringForType:NSPasteboardTypeHTML]));
            }
            if (wanted(RCS_CLIPBOARD_RTF, [types containsObject:NSPasteboardTypeRTF])) {
                add(RCS_CLIPBOARD_RTF, bytesOf([board dataForType:NSPasteboardTypeRTF]));
            }
            const bool png = [types containsObject:NSPasteboardTypePNG];
            // Beside files Finder puts a picture of their icons: not an image copied.
            if (!fileUrls && wanted(RCS_CLIPBOARD_IMAGE, png || [types containsObject:NSPasteboardTypeTIFF])) {
                NSData* image = png ? [board dataForType:NSPasteboardTypePNG] : nil;
                if (!image) {
                    if (NSData* tiff = [board dataForType:NSPasteboardTypeTIFF]) {
                        image = converted(tiff, CFSTR("public.png"));
                    }
                }
                add(RCS_CLIPBOARD_IMAGE, bytesOf(image));
            }
            if (wanted(RCS_CLIPBOARD_FILES, fileUrls || promised)) {
                auto files = fileUrls ? filesOf(board) : std::nullopt;
                if (files && filesBytes(*files) <= left) {
                    left -= filesBytes(*files);
                    content.files = std::move(files);
                } else {
                    content.leftOut |= filesFormat; // Too many, gone, or another program's promise.
                }
            }
            return content;
        }
    }

    uint64_t write(const Content& content) override {
        @autoreleasepool {
            NSPasteboard* board = board_;
            const bool files = content.files.has_value();
            if (files) { // On this Mac alone: not to the user's other devices.
                [board prepareForNewContentsWithOptions:NSPasteboardContentsCurrentHostOnly];
            } else {
                [board clearContents];
            }
            bool placed = true;
            if (files) {
                standIns_.reset();
                if (!content.files->entries.front().path.empty()) { // The files themselves.
                    NSMutableArray<NSURL*>* urls = [NSMutableArray array];
                    for (const auto& entry : content.files->entries) {
                        [urls addObject:[NSURL fileURLWithFileSystemRepresentation:entry.path.c_str()
                                                                       isDirectory:entry.kind == RCS_FILE_DIRECTORY
                                                                     relativeToURL:nil]];
                    }
                    placed = [board writeObjects:urls];
                } else {
                    standIns_ = std::make_unique<StandIn>(*content.files, pastes_);
                    placed = standIns_->place(board);
                }
            }
            for (const auto& item : content.items) {
                NSData* data = [NSData dataWithBytes:item.bytes.data() length:item.bytes.size()];
                const auto setText = [&](NSPasteboardType type) {
                    NSString* text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
                    return text && [board setString:text forType:type];
                };
                switch (item.format) {
                case RCS_CLIPBOARD_TEXT:
                    placed = setText(NSPasteboardTypeString) && placed;
                    break;
                case RCS_CLIPBOARD_HTML:
                    placed = setText(NSPasteboardTypeHTML) && placed;
                    break;
                case RCS_CLIPBOARD_RTF:
                    placed = [board setData:data forType:NSPasteboardTypeRTF] && placed;
                    break;
                case RCS_CLIPBOARD_IMAGE:
                    placed = [board setData:data forType:NSPasteboardTypePNG] && placed;
                    if (NSData* tiff = converted(data, CFSTR("public.tiff"))) {
                        [board setData:tiff forType:NSPasteboardTypeTIFF];
                    }
                    break;
                default:
                    break;
                }
            }
            if (content.concealed) {
                [board setData:[NSData data] forType:concealedType];
            }
            if (files) {
                [board setData:[NSData data] forType:transientType];
            }
            if (!placed) {
                fail(RCS_ERROR_DEVICE, "The pasteboard did not take the copy");
            }
            return uint64_t(board.changeCount);
        }
    }

  private:
    NSPasteboard* const board_;
    std::unique_ptr<StandIn> standIns_; // The other end's files placed last.
};

} // namespace

std::unique_ptr<SystemClipboard> createPlatformClipboard() {
    return std::make_unique<MacClipboard>(NSPasteboard.generalPasteboard);
}

std::unique_ptr<SystemClipboard> createPasteboardClipboard(void* pasteboard) {
    return std::make_unique<MacClipboard>((__bridge NSPasteboard*)pasteboard);
}

uint32_t platformFormats() noexcept {
    return allFormats;
}

bool platformCarriesFiles() noexcept {
    return true;
}

uint32_t platformFiles(std::string& why) {
    if (std::getenv("APP_SANDBOX_CONTAINER_ID")) {
        why = "A sandboxed application reads no file another one copied, and writes none where Finder pastes";
        return 0;
    }
    if (!mainRunLoopRuns()) {
        why = "This process does not run its main run loop, where macOS asks for the files pasted";
        return RCS_CLIPBOARD_SEND;
    }
    return RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE;
}

} // namespace rcsdk::clipboard
