// The system clipboard (SystemClipboard) on iOS: the general pasteboard. Text
// is public.utf8-plain-text, HTML public.html, RTF public.rtf; an image
// public.png, else public.jpeg, public.heic or public.tiff (converted with
// ImageIO), placed as PNG. A copy is concealed where its program marked it
// with the nspasteboard.org types (as macOS ones do, also across Universal
// Clipboard), and placed only on this device (localOnly) with ConcealedType.
//
// iOS asks the user before an application reads what another one copied
// ("Allow Paste", iOS 16 on, unless allowed in Settings): each read() of a
// new copy may show it. The change count and the types never do. UIPasteboard
// is used from the caller's thread.
//
// Files (FILES): item providers with a name and a file representation (a
// file's, or a folder's) are another application's files, which may be gone
// once it is in the background: they are copied into the files directory as
// the copy is read (30 s at most each), counted against max_bytes. Files
// placed whole are providers of those files; the other end's are providers
// whose file representations fetch the copy's files into the files directory
// when an application loads them (once for all of them), with a progress that
// cancels the fetch. Either is placed on this device only.
#include "pastes.hpp"

#import <ImageIO/ImageIO.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cstring>
#include <mutex>

namespace rcsdk::clipboard {
namespace {

NSString* const concealedType = @"org.nspasteboard.ConcealedType";
NSString* const transientType = @"org.nspasteboard.TransientType";
NSString* const textType = @"public.utf8-plain-text";
NSString* const htmlType = @"public.html";
NSString* const rtfType = @"public.rtf";
NSString* const pngType = @"public.png";

Bytes bytesOf(NSData* data) {
    if (!data) {
        return {};
    }
    const auto* begin = static_cast<const uint8_t*>(data.bytes);
    return Bytes(begin, begin + data.length);
}

// The UTF-8 of a text type, whether the pasteboard holds it as data or as a string.
Bytes textOf(UIPasteboard* board, NSString* type) {
    id value = [board valueForPasteboardType:type];
    if ([value isKindOfClass:NSString.class]) {
        const char* utf8 = [(NSString*)value UTF8String];
        return utf8 ? Bytes(utf8, utf8 + std::strlen(utf8)) : Bytes{};
    }
    return bytesOf([board dataForPasteboardType:type]);
}

// An image's bytes as PNG; nil where ImageIO cannot read them.
NSData* png(NSData* image) {
    CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)image, nullptr);
    if (!source) {
        return nil;
    }
    CGImageRef picture = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    CFRelease(source);
    if (!picture) {
        return nil;
    }
    NSMutableData* out = [NSMutableData data];
    CGImageDestinationRef destination =
        CGImageDestinationCreateWithData((__bridge CFMutableDataRef)out, CFSTR("public.png"), 1, nullptr);
    bool done = false;
    if (destination) {
        CGImageDestinationAddImage(destination, picture, nullptr);
        done = CGImageDestinationFinalize(destination);
        CFRelease(destination);
    }
    CGImageRelease(picture);
    return done ? out : nil;
}

// The bytes of what is at `path` (a folder's, all it holds).
uint64_t sizeAt(NSString* path) {
    NSFileManager* files = NSFileManager.defaultManager;
    BOOL folder = NO;
    if (![files fileExistsAtPath:path isDirectory:&folder]) {
        return 0;
    }
    if (!folder) {
        return [[files attributesOfItemAtPath:path error:nil] fileSize];
    }
    uint64_t total = 0;
    for (NSString* inside in [files enumeratorAtPath:path]) {
        total += [[files attributesOfItemAtPath:[path stringByAppendingPathComponent:inside] error:nil] fileSize];
    }
    return total;
}

// The files another application copied, copied into `directory` while
// their bytes fit `left`; nullopt without any. `tooMany`: some did not fit.
std::optional<Files> copiedFiles(UIPasteboard* board, Pastes& pastes, size_t& left, bool& tooMany) {
    Files files;
    std::string directory;
    for (NSItemProvider* provider in board.itemProviders) {
        NSString* name = provider.suggestedName;
        NSString* type = nil;
        bool folder = false;
        for (NSString* id in provider.registeredTypeIdentifiers) {
            UTType* known = [UTType typeWithIdentifier:id];
            if (!type && known && ([known conformsToType:UTTypeFolder] || [known conformsToType:UTTypeData])) {
                type = id;
                folder = [known conformsToType:UTTypeFolder];
            }
        }
        if (!name || !type || !nameProblem(name.UTF8String).empty()) {
            continue; // Text or a picture copied, not a file.
        }
        if (directory.empty()) {
            Publication local{};
            uint8_t random[32];
            rcs_generate_connection_key(random, sizeof(random));
            std::copy(random, random + local.size(), local.begin());
            directory = pastes.directoryFor(local);
        }
        NSString* target = [@(directory.c_str()) stringByAppendingPathComponent:name];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block bool copied = false;
        [provider loadFileRepresentationForTypeIdentifier:type
                                        completionHandler:^(NSURL* url, NSError*) {
                                          // The file is the provider's only during the call.
                                          copied = url && [NSFileManager.defaultManager copyItemAtURL:url
                                                                                                toURL:[NSURL fileURLWithPath:target]
                                                                                                error:nil];
                                          dispatch_semaphore_signal(done);
                                        }];
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, int64_t(30) * NSEC_PER_SEC)) || !copied) {
            continue;
        }
        const uint64_t size = sizeAt(target);
        if (size > left) {
            [NSFileManager.defaultManager removeItemAtPath:target error:nil];
            tooMany = true;
            continue;
        }
        left -= size_t(size);
        files.entries.push_back({name.UTF8String, folder ? RCS_FILE_DIRECTORY : RCS_FILE_REGULAR,
                                 folder ? 0 : size, 0, target.fileSystemRepresentation});
    }
    return files.entries.empty() ? std::nullopt : std::optional(std::move(files));
}

// The other end's files as item providers: the first application that
// loads one fetches them all into the files directory.
struct StandIns {
    Files files;
    std::shared_ptr<Pastes> pastes;
    std::mutex mutex;
    std::shared_ptr<PasteFetch> fetch; // The newest.
    std::string directory;

    // The fetch going, or done well, or a new one; nullptr with why.
    std::shared_ptr<PasteFetch> start(std::string& why) {
        std::lock_guard lock(mutex);
        if (fetch) {
            std::lock_guard fetching(fetch->mutex);
            if (!fetch->ended || (fetch->ended->result == RCS_OK && fetch->ended->complete)) {
                return fetch;
            }
        }
        if (pastes->revoked(files.placing)) {
            why = "The copy's files were withdrawn: the other end copied something else since";
            pastes->refused(files.placing, RCS_ERROR_INVALID_STATE, why);
            return nullptr;
        }
        try {
            directory = pastes->directoryFor(files.publication);
        } catch (const Error& error) {
            why = error.what();
            pastes->refused(files.placing, error.result(), why);
            return nullptr;
        }
        fetch = pastes->fetch(files.placing, RCS_FILE_FETCH_INTO, directory);
        return fetch;
    }
};

NSError* errorOf(rcs_result result, const std::string& why) {
    return [NSError errorWithDomain:@"rcsdk.clipboard"
                               code:result
                           userInfo:@{NSLocalizedDescriptionKey : @(why.c_str())}];
}

NSItemProvider* standInProvider(const std::shared_ptr<StandIns>& state, const FileEntry& entry) {
    NSItemProvider* provider = [[NSItemProvider alloc] init];
    NSString* name = @(entry.name.c_str());
    provider.suggestedName = name;
    UTType* type = entry.kind == RCS_FILE_DIRECTORY ? UTTypeFolder : [UTType typeWithFilenameExtension:name.pathExtension];
    if (!type) {
        type = UTTypeData;
    }
    [provider registerFileRepresentationForTypeIdentifier:type.identifier
                                              fileOptions:0
                                               visibility:NSItemProviderRepresentationVisibilityAll
                                              loadHandler:^NSProgress*(void (^completion)(NSURL*, BOOL, NSError*)) {
                                                NSProgress* progress = [NSProgress progressWithTotalUnitCount:1];
                                                std::string why;
                                                const auto fetch = state->start(why);
                                                if (!fetch) {
                                                    completion(nil, NO, errorOf(RCS_ERROR_INVALID_STATE, why));
                                                    return progress;
                                                }
                                                const auto pastes = state->pastes;
                                                progress.cancellationHandler = ^{
                                                  pastes->close(*fetch, RCS_ERROR_CANCELLED);
                                                };
                                                NSString* path =
                                                    [@(state->directory.c_str()) stringByAppendingPathComponent:name];
                                                dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                                                  fetch->waitFor([&] { return fetch->ended.has_value(); },
                                                                 std::chrono::hours(24));
                                                  std::optional<EndedMessage> ended;
                                                  {
                                                      std::lock_guard lock(fetch->mutex);
                                                      ended = fetch->ended;
                                                  }
                                                  if (ended && ended->result == RCS_OK) {
                                                      completion([NSURL fileURLWithPath:path], NO, nil);
                                                  } else {
                                                      completion(nil, NO,
                                                                 errorOf(ended ? ended->result : RCS_ERROR_TIMEOUT,
                                                                         ended ? ended->message
                                                                               : "The files did not come"));
                                                  }
                                                  progress.completedUnitCount = 1;
                                                });
                                                return progress;
                                              }];
    return provider;
}

class IosClipboard final : public SystemClipboard {
  public:
    explicit IosClipboard(UIPasteboard* board) : board_(board) {}

    uint64_t changeCount() override {
        @autoreleasepool {
            return uint64_t(board_.changeCount);
        }
    }

    Content read(uint32_t formats, size_t maxBytes) override {
        @autoreleasepool {
            UIPasteboard* board = board_;
            NSArray<NSString*>* types = board.pasteboardTypes ? board.pasteboardTypes : @[];
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
            if (wanted(RCS_CLIPBOARD_TEXT, [types containsObject:textType] || [types containsObject:@"public.text"])) {
                add(RCS_CLIPBOARD_TEXT, [types containsObject:textType] ? textOf(board, textType)
                                                                         : textOf(board, @"public.text"));
            }
            if (wanted(RCS_CLIPBOARD_HTML, [types containsObject:htmlType])) {
                add(RCS_CLIPBOARD_HTML, textOf(board, htmlType));
            }
            if (wanted(RCS_CLIPBOARD_RTF, [types containsObject:rtfType])) {
                add(RCS_CLIPBOARD_RTF, bytesOf([board dataForPasteboardType:rtfType]));
            }
            NSString* other = nil; // An image in another format, converted.
            for (NSString* type in @[ @"public.jpeg", @"public.heic", @"public.tiff" ]) {
                if (!other && [types containsObject:type]) {
                    other = type;
                }
            }
            const bool isPng = [types containsObject:pngType];
            if (wanted(RCS_CLIPBOARD_IMAGE, isPng || other)) {
                NSData* image = isPng ? [board dataForPasteboardType:pngType] : nil;
                if (!image && other) {
                    if (NSData* data = [board dataForPasteboardType:other]) {
                        image = png(data);
                    }
                }
                add(RCS_CLIPBOARD_IMAGE, bytesOf(image));
            }
            if (formats & filesFormat) {
                bool tooMany = false;
                auto files = copiedFiles(board, *pastes_, left, tooMany);
                if (files && filesBytes(*files) <= left) {
                    content.files = std::move(files);
                }
                if (tooMany || (files && !content.files)) {
                    content.leftOut |= filesFormat;
                }
            }
            return content;
        }
    }

    uint64_t write(const Content& content) override {
        @autoreleasepool {
            NSMutableDictionary<NSString*, id>* item = [NSMutableDictionary dictionary];
            for (const auto& part : content.items) {
                NSData* data = [NSData dataWithBytes:part.bytes.data() length:part.bytes.size()];
                const auto setText = [&](NSString* type) {
                    if (NSString* text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]) {
                        item[type] = text;
                    }
                };
                switch (part.format) {
                case RCS_CLIPBOARD_TEXT:
                    setText(textType);
                    break;
                case RCS_CLIPBOARD_HTML:
                    setText(htmlType);
                    break;
                case RCS_CLIPBOARD_RTF:
                    item[rtfType] = data;
                    break;
                case RCS_CLIPBOARD_IMAGE:
                    item[pngType] = data;
                    break;
                default:
                    break;
                }
            }
            if (item.count != content.items.size()) {
                fail(RCS_ERROR_DEVICE, "The pasteboard did not take the copy: text that is not UTF-8");
            }
            if (content.files) { // Providers of files, beside one of the rest: on this device only.
                NSMutableArray<NSItemProvider*>* providers = [NSMutableArray array];
                if (content.files->entries.front().path.empty()) {
                    const auto state = std::make_shared<StandIns>();
                    state->files = *content.files;
                    state->pastes = pastes_;
                    for (const auto& entry : content.files->entries) {
                        [providers addObject:standInProvider(state, entry)];
                    }
                } else {
                    for (const auto& entry : content.files->entries) {
                        NSItemProvider* provider =
                            [[NSItemProvider alloc] initWithContentsOfURL:[NSURL fileURLWithPath:@(entry.path.c_str())]];
                        if (provider) {
                            [providers addObject:provider];
                        }
                    }
                }
                for (NSString* type in item) {
                    NSItemProvider* beside = [[NSItemProvider alloc] init];
                    id value = item[type];
                    [beside registerDataRepresentationForTypeIdentifier:type
                                                             visibility:NSItemProviderRepresentationVisibilityAll
                                                            loadHandler:^NSProgress*(void (^completion)(NSData*, NSError*)) {
                                                              completion([value isKindOfClass:NSString.class]
                                                                             ? [(NSString*)value dataUsingEncoding:NSUTF8StringEncoding]
                                                                             : (NSData*)value,
                                                                         nil);
                                                              return nil;
                                                            }];
                    [providers addObject:beside];
                }
                [board_ setItemProviders:providers localOnly:YES expirationDate:nil];
                return uint64_t(board_.changeCount);
            }
            NSDictionary* options = @{};
            if (content.concealed) {
                item[concealedType] = [NSData data];
                options = @{UIPasteboardOptionLocalOnly : @YES}; // Not to the user's other devices.
            }
            [board_ setItems:item.count ? @[ item ] : @[] options:options];
            return uint64_t(board_.changeCount);
        }
    }

  private:
    UIPasteboard* const board_;
};

} // namespace

std::unique_ptr<SystemClipboard> createPlatformClipboard() {
    return std::make_unique<IosClipboard>(UIPasteboard.generalPasteboard);
}

std::unique_ptr<SystemClipboard> createPasteboardClipboard(void* pasteboard) {
    return std::make_unique<IosClipboard>((__bridge UIPasteboard*)pasteboard);
}

uint32_t platformFormats() noexcept {
    return allFormats;
}

bool platformCarriesFiles() noexcept {
    return true;
}

uint32_t platformFiles(std::string&) {
    return RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE;
}

} // namespace rcsdk::clipboard
