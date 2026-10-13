#include "stand_in_macos.hpp"

#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace rcsdk::clipboard {
namespace {

std::mutex seamsMutex;
std::function<std::string()> pastingSeam;
std::vector<std::string> rootsSeam;

std::string hex(const Publication& publication) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (const uint8_t byte : publication) {
        out += digits[byte >> 4];
        out += digits[byte & 15];
    }
    return out;
}

std::string pastingApplication() {
    {
        std::lock_guard lock(seamsMutex);
        if (pastingSeam) {
            return pastingSeam();
        }
    }
    @autoreleasepool {
        NSString* id = NSWorkspace.sharedWorkspace.frontmostApplication.bundleIdentifier;
        return id ? id.UTF8String : "";
    }
}

std::vector<std::string> watchedRoots() {
    {
        std::lock_guard lock(seamsMutex);
        if (!rootsSeam.empty()) {
            return rootsSeam;
        }
    }
    @autoreleasepool {
        return {NSHomeDirectory().fileSystemRepresentation, "/Volumes"};
    }
}

// A placeholder there: a file of its own, of 0 bytes.
bool placeholderAt(const std::string& path) {
    struct stat s {};
    return lstat(path.c_str(), &s) == 0 && S_ISREG(s.st_mode) && s.st_size == 0;
}

} // namespace

void setStandInSeams(std::function<std::string()> pastingApplication, std::vector<std::string> roots) {
    std::lock_guard lock(seamsMutex);
    pastingSeam = std::move(pastingApplication);
    rootsSeam = std::move(roots);
}

bool mainRunLoopRuns() {
    if (NSThread.isMainThread) {
        return true; // Asked there, it runs it when the caller returns.
    }
    const auto ran = std::make_shared<std::atomic<bool>>(false);
    dispatch_async(dispatch_get_main_queue(), ^{
      *ran = true;
    });
    for (int i = 0; i < 100 && !*ran; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return *ran;
}

// What the provider and the FSEvents stream share with the StandIn.
struct StandIn::State {
    Files files;
    std::shared_ptr<Pastes> pastes;
    std::string name;   // ".rcsdk-paste-<publication>"
    std::mutex mutex;
    bool closed = false; // Its StandIn went: another copy took its place here.
    std::string source; // The placeholder given, once made.
    FSEventStreamRef stream = nullptr;
    dispatch_queue_t queue = nullptr;
    std::set<std::string> found; // Copies of it seen, by path, until their fetch ended.
    std::weak_ptr<State> weak;   // Itself, for what outlives a call.

    // The provider: a file URL for Finder (the placeholder, watched for from
    // now on), nothing for another program.
    NSString* provide() {
        const std::string pasting = pastingApplication();
        if (!pastes->standIn()) {
            pastes->refused(files.placing, RCS_ERROR_UNSUPPORTED,
                            "files_stand_in is 0: only files fetched ahead are pasted on this Mac");
            return nil;
        }
        if (pasting != "com.apple.finder") {
            pastes->refused(files.placing, RCS_ERROR_UNSUPPORTED,
                            "The program that pastes (" + (pasting.empty() ? std::string("unknown") : pasting) +
                                ") takes no files of the other end's on this Mac: paste them in Finder");
            return nil;
        }
        if (pastes->revoked(files.placing) || isClosed()) {
            pastes->refused(files.placing, RCS_ERROR_INVALID_STATE,
                            "The copy's files were withdrawn: the other end copied something else since");
            return nil;
        }
        std::string path;
        try {
            path = placeholder();
        } catch (const Error& error) {
            pastes->refused(files.placing, error.result(), error.what());
            return nil;
        }
        watch();
        @autoreleasepool {
            return [NSURL fileURLWithFileSystemRepresentation:path.c_str() isDirectory:NO relativeToURL:nil]
                .absoluteString;
        }
    }

    bool isClosed() {
        std::lock_guard lock(mutex);
        return closed;
    }

    // The placeholder, made where it is not (moved away by a paste).
    std::string placeholder() {
        std::lock_guard lock(mutex);
        if (source.empty()) {
            source = pastes->directoryFor(files.publication) + "/" + name;
        }
        if (!placeholderAt(source)) {
            const int fd = ::open(source.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd < 0 && errno != EEXIST) {
                fail(RCS_ERROR_DEVICE, "Cannot make the placeholder " + source + ": " + std::strerror(errno));
            }
            if (fd >= 0) {
                ::close(fd);
            }
        }
        return source;
    }

    void watch() {
        std::lock_guard lock(mutex);
        if (stream || closed) {
            return;
        }
        NSMutableArray<NSString*>* paths = [NSMutableArray array];
        for (const auto& root : watchedRoots()) {
            [paths addObject:[NSString stringWithUTF8String:root.c_str()]];
        }
        FSEventStreamContext context{};
        context.info = this;
        stream = FSEventStreamCreate(nullptr, &State::events, &context, (__bridge CFArrayRef)paths,
                                     kFSEventStreamEventIdSinceNow, 0.1,
                                     kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
        if (!stream) {
            return;
        }
        queue = dispatch_queue_create("rcsdk.clipboard.stand-in", DISPATCH_QUEUE_SERIAL);
        FSEventStreamSetDispatchQueue(stream, queue);
        FSEventStreamStart(stream);
    }

    void unwatch() {
        FSEventStreamRef going = nullptr;
        {
            std::lock_guard lock(mutex);
            std::swap(going, stream);
        }
        if (going) { // Its callbacks are through once it is invalidated, and the one under way returned.
            FSEventStreamStop(going);
            FSEventStreamInvalidate(going);
            FSEventStreamRelease(going);
            dispatch_sync(queue, ^{
                          });
        }
    }

    static void events(ConstFSEventStreamRef, void* info, size_t count, void* paths,
                       const FSEventStreamEventFlags flags[], const FSEventStreamEventId[]) {
        auto* self = static_cast<State*>(info);
        auto* const* list = static_cast<const char* const*>(paths);
        for (size_t i = 0; i < count; ++i) {
            const std::string path = list[i];
            const size_t slash = path.rfind('/');
            // By its name alone: nothing else is looked at.
            if (slash == std::string::npos || path.compare(slash + 1, std::string::npos, self->name) != 0 ||
                !(flags[i] & (kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemRenamed))) {
                continue;
            }
            self->seen(path, path.substr(0, slash));
        }
    }

    // A copy of the placeholder (or the placeholder moved) at `path`, in `directory`.
    void seen(const std::string& path, const std::string& directory) {
        {
            std::lock_guard lock(mutex);
            if (path == source || !found.insert(path).second) {
                return;
            }
        }
        if (!placeholderAt(path) || pastes->revoked(files.placing)) {
            return; // Gone again, or not the placeholder (a file of that name with something in it).
        }
        try {
            placeholder(); // Moved there: given again where it was.
        } catch (const Error&) {
        }
        const auto fetch = pastes->fetch(files.placing, RCS_FILE_FETCH_INTO, directory);
        // Once the fetch ended, the placeholder copied goes (still a
        // placeholder: 0 bytes, of that name), and a paste there again is seen.
        const auto tidy = [fetch, path, self = weak] {
            {
                std::lock_guard lock(fetch->mutex);
                if (!fetch->ended) {
                    return;
                }
            }
            if (placeholderAt(path)) {
                ::unlink(path.c_str());
            }
            if (const auto state = self.lock()) {
                std::lock_guard lock(state->mutex);
                state->found.erase(path);
            }
        };
        {
            std::lock_guard lock(fetch->mutex);
            fetch->wake = tidy;
        }
        tidy();
    }
};

StandIn::StandIn(const Files& files, std::shared_ptr<Pastes> pastes) : state_(std::make_shared<State>()) {
    state_->weak = state_;
    state_->files = files;
    state_->pastes = std::move(pastes);
    state_->name = ".rcsdk-paste-" + hex(files.publication);
}

StandIn::~StandIn() {
    {
        std::lock_guard lock(state_->mutex);
        state_->closed = true;
    }
    state_->unwatch();
    std::lock_guard lock(state_->mutex);
    if (!state_->source.empty() && placeholderAt(state_->source)) {
        ::unlink(state_->source.c_str());
    }
}

}

// The pasteboard's data provider: the placeholder's URL as it is asked for.
@interface RcsdkStandInProvider : NSObject <NSPasteboardItemDataProvider>
- (instancetype)initWithState:(std::shared_ptr<rcsdk::clipboard::StandIn::State>)state;
@end

@implementation RcsdkStandInProvider {
    std::shared_ptr<rcsdk::clipboard::StandIn::State> state_;
}

- (instancetype)initWithState:(std::shared_ptr<rcsdk::clipboard::StandIn::State>)state {
    if ((self = [super init])) {
        state_ = std::move(state);
    }
    return self;
}

- (void)pasteboard:(NSPasteboard*)pasteboard item:(NSPasteboardItem*)item provideDataForType:(NSPasteboardType)type {
    if ([type isEqualToString:NSPasteboardTypeFileURL]) {
        if (NSString* url = state_->provide()) {
            [item setString:url forType:type];
        }
    }
}

@end

namespace rcsdk::clipboard {

bool StandIn::place(NSPasteboard* board) {
    @autoreleasepool {
        NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
        RcsdkStandInProvider* provider = [[RcsdkStandInProvider alloc] initWithState:state_];
        return [item setDataProvider:provider forTypes:@[ NSPasteboardTypeFileURL ]] && [board writeObjects:@[ item ]];
    }
}

} // namespace rcsdk::clipboard
