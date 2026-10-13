// Screen capture with ScreenCaptureKit: a display, a region of one (the
// stream's source rectangle), one window (a desktop-independent window
// filter: covered or off-screen parts still show) or an application's windows
// on a display (an including-applications filter). Frames arrive on a serial
// dispatch queue and replace a single "latest frame" slot that next()
// consumes at the configured rate. Frames come in the encoder's YCbCr layout
// (NV12 by default, BT.709; 4:4:4 and 10-bit as the stream's picture format
// says): VideoToolbox cannot convert BGRA fast enough for 4K at 120 FPS (HEVC
// latency 18 ms with NV12, 91 ms with BGRA). With the cursor sent apart, ScreenCaptureKit leaves
// it out and a CursorTracker reports it at every next().
//
// As on Windows, a running capture pauses while the display cannot be
// captured and resumes by itself: when the display sleeps or goes blank
// (frames say so), when the stream stops (the screen locked: ScreenCaptureKit
// then lists no display), and when the display's mode or rotation changes
// (the stream starts again to place the display anew). While paused, it
// tries to start a stream every 250 ms. A display disconnected for 10
// seconds, a stream the user or the system's privacy settings stopped, and a
// stream that keeps refusing to start fail capture with the reason.
//
// A window or application that does not show pauses capture too: the window
// server says whether a window is on screen and whether it still exists (a
// window's stream delivers suspended frames meanwhile), and whether an
// application has windows on the display (its stream would show black, so it
// stops until one shows). Once the window or the process is gone, capture
// stays paused. A window's stream follows its size: each frame says where the
// window sits in it, and the stream is told the window's new place.
#include "platform/apple/cf.hpp"
#include "platform/macos/cursor_tracker.hpp"
#include "platform/macos/window_server.hpp"
#include "platform/platform.hpp"
#include "util/log.hpp"

#include <rcsdk/view_geometry.hpp>

#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unistd.h>

namespace rcsdk::platform::mac {
struct CaptureState {
    std::mutex mutex;
    std::condition_variable changed;
    VideoFrame latest;
    bool hasLatest = false;
    // Where the latest frame shows what is shared, in its pixels, and where
    // that is on screen (global points): what a window's frames say.
    CGRect latestContent = CGRectNull;
    CGRect latestScreen = CGRectNull;
    bool interrupted = false;
    bool closed = false;
    const void* stream = nullptr; // The stream whose output counts.
    NSError* stopped = nil;       // It stopped with this.
    std::string failure;          // Capture cannot go on.
    bool blank = false;           // Its display sleeps or is blank, or its window does not show: no pictures.
    uint64_t lastFrameNs = 0;     // When the latest picture came.
    uint64_t frames = 0;
    uint64_t overwritten = 0;
    uint64_t interruptions = 0;
};
} // namespace rcsdk::platform::mac

using rcsdk::platform::mac::CaptureState;

@interface RCSCaptureSink : NSObject <SCStreamOutput, SCStreamDelegate>
- (instancetype)initWithState:(std::shared_ptr<CaptureState>)state;
@end

@implementation RCSCaptureSink {
    std::shared_ptr<CaptureState> _state;
}

- (instancetype)initWithState:(std::shared_ptr<CaptureState>)state {
    if ((self = [super init])) {
        _state = std::move(state);
    }
    return self;
}

- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sample) || !CMSampleBufferDataIsReady(sample)) {
        return;
    }
    NSArray* attachments = (__bridge NSArray*)CMSampleBufferGetSampleAttachmentsArray(sample, false);
    NSDictionary* info = attachments.firstObject;
    NSNumber* status = info[SCStreamFrameInfoStatus];
    const auto frameStatus = status ? SCFrameStatus(status.integerValue) : SCFrameStatusComplete;
    if (frameStatus == SCFrameStatusBlank || frameStatus == SCFrameStatusSuspended) {
        std::lock_guard lock(_state->mutex);
        if ((__bridge const void*)stream == _state->stream && !_state->blank) {
            _state->blank = true;
            ++_state->interruptions;
            _state->changed.notify_one();
        }
        return;
    }
    // Idle and stopped notifications carry no new screen content.
    CVPixelBufferRef buffer = CMSampleBufferGetImageBuffer(sample);
    if ((frameStatus != SCFrameStatusComplete && frameStatus != SCFrameStatusStarted) || !buffer) {
        return;
    }
    CGRect content = CGRectNull, screen = CGRectNull;
    if (NSDictionary* value = info[SCStreamFrameInfoContentRect]) { // In points of the frame.
        CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)value, &content);
        const double scale = [info[SCStreamFrameInfoScaleFactor] doubleValue];
        content = CGRectMake(content.origin.x * scale, content.origin.y * scale, content.size.width * scale,
                             content.size.height * scale);
    }
    if (NSDictionary* value = info[SCStreamFrameInfoScreenRect]) {
        CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)value, &screen);
    }
    rcsdk::VideoFrame frame;
    frame.surface = rcsdk::platform::apple::retainSurface(buffer);
    frame.width = static_cast<uint32_t>(CVPixelBufferGetWidth(buffer));
    frame.height = static_cast<uint32_t>(CVPixelBufferGetHeight(buffer));
    frame.timestampNs = rcsdk::monotonicNs();
    std::lock_guard lock(_state->mutex);
    if (_state->closed || (__bridge const void*)stream != _state->stream) {
        return;
    }
    _state->blank = false;
    _state->lastFrameNs = frame.timestampNs;
    frame.sequence = ++_state->frames;
    _state->overwritten += _state->hasLatest;
    _state->latest = std::move(frame);
    _state->latestContent = content;
    _state->latestScreen = screen;
    _state->hasLatest = true;
    _state->changed.notify_one();
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    std::lock_guard lock(_state->mutex);
    if (!_state->closed && (__bridge const void*)stream == _state->stream) {
        _state->stopped = error ? error
                                : [NSError errorWithDomain:SCStreamErrorDomain code:SCStreamErrorInternalError
                                                  userInfo:nil];
        _state->changed.notify_all();
    }
}
@end

namespace rcsdk::platform {
namespace {

constexpr auto retryInterval = 250ms;
constexpr auto disconnectedLimit = 10s; // A display offline that long is gone.
constexpr int refusedStarts = 40;        // 10 s of starts the stream refused.

// Waits for a completion handler, polling for cancellation; `late` (with
// what) once `deadline` has passed.
void await(dispatch_semaphore_t done, CancelToken cancelled, const char* operation,
           Clock::time_point deadline = Clock::now() + 8s, rcs_result late = RCS_ERROR_CAPTURE) {
    while (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC))) {
        throwIfCancelled(cancelled);
        if (Clock::now() >= deadline) {
            fail(late, std::string(operation) + " timed out");
        }
    }
}

std::string text(NSError* error) {
    const char* description = error.localizedDescription.UTF8String;
    return description ? description : "no reason given";
}

void throwIfError(NSError* error, const char* operation) {
    if (error) {
        fail(RCS_ERROR_CAPTURE, std::string(operation) + ": " + text(error));
    }
}

// Fails capture for a stop that is no passing state of the display: the
// user or the system's privacy settings ended it. Other stops pause.
void failIfFinal(NSError* error) {
    if (![error.domain isEqualToString:SCStreamErrorDomain]) {
        return;
    }
    switch (error.code) {
    case SCStreamErrorUserDeclined:
        fail(RCS_ERROR_PERMISSION_REQUIRED, "Screen capture is not permitted: " + text(error));
    case SCStreamErrorUserStopped:
        fail(RCS_ERROR_CAPTURE, "Screen capture was stopped from the system's screen sharing controls");
    case SCStreamErrorMissingEntitlements:
    case SCStreamErrorInvalidParameter:
        fail(RCS_ERROR_CAPTURE, "Screen capture stopped: " + text(error));
    default:
        return;
    }
}

bool online(CGDirectDisplayID display) {
    uint32_t count = 0;
    CGDirectDisplayID displays[32];
    if (CGGetOnlineDisplayList(32, displays, &count) != kCGErrorSuccess) {
        return true; // Unknown: the next attempt to start tells.
    }
    return std::find(displays, displays + count, display) != displays + count;
}

Rect rounded(CGRect rect) {
    return {uint32_t(std::lround(rect.origin.x)), uint32_t(std::lround(rect.origin.y)),
            uint32_t(std::lround(rect.size.width)), uint32_t(std::lround(rect.size.height))};
}

// What ScreenCaptureKit lists now, or why it does not. For a window, windows
// off screen too: one that just went is found, and then shows again.
struct Listed {
    SCShareableContent* content = nil;
    NSError* error = nil;
};
Listed listContent(bool window, CancelToken cancelled, Clock::time_point deadline = Clock::now() + 8s,
                   rcs_result late = RCS_ERROR_CAPTURE) {
    __block Listed listed;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    const auto handler = ^(SCShareableContent* content, NSError* error) {
        listed.content = content;
        listed.error = error;
        dispatch_semaphore_signal(done);
    };
    if (window) {
        [SCShareableContent getShareableContentExcludingDesktopWindows:YES
                                                   onScreenWindowsOnly:NO
                                                     completionHandler:handler];
    } else {
        [SCShareableContent getShareableContentWithCompletionHandler:handler];
    }
    await(done, cancelled, "Listing shareable content", deadline, late);
    return listed;
}

// What a capture of `target` shows, as `content` lists it: the filter, and
// the display it is on (DISPLAY, REGION, APPLICATION: `displayId`, its mode
// now, and for a region the part of it shared, in its pixels) or the window.
// No filter while there is none to make: `hidden` says why, NONE while the
// display is not listed (the screen locked or asleep, the display reconfigured).
struct Shareable {
    SCContentFilter* filter = nil;
    SCDisplay* display = nil;
    DisplayMode mode;
    Rect region;
    SCWindow* window = nil;
    rcs_capture_pause hidden = RCS_CAPTURE_PAUSE_NONE;
};
Shareable shareable(SCShareableContent* content, const CaptureTarget& target, CGDirectDisplayID displayId,
                    bool excludeCurrentProcess) {
    Shareable shared;
    if (target.source == RCS_CAPTURE_WINDOW) {
        for (SCWindow* window in content.windows) {
            if (window.windowID == CGWindowID(target.windowId)) {
                shared.window = window;
                shared.filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:window];
                return shared;
            }
        }
        shared.hidden =
            mac::windowState(CGWindowID(target.windowId)) ? RCS_CAPTURE_PAUSE_HIDDEN : RCS_CAPTURE_PAUSE_GONE;
        return shared;
    }
    for (SCDisplay* candidate in content.displays) {
        if (candidate.displayID == displayId) {
            shared.display = candidate;
        }
    }
    if (!shared.display) {
        return shared;
    }
    // Its pixels and its points (SCDisplay's, as CGDisplayBounds) both in the
    // desktop's orientation, also for a display turned to portrait.
    const auto geometry = mac::displayGeometry(displayId);
    shared.mode = {geometry.width, geometry.height, geometry.refreshHz};
    if (target.source == RCS_CAPTURE_REGION) {
        // The region as far as the display reaches now: a mode change may have
        // left part of it off the display.
        const auto& r = target.region;
        const uint32_t right = std::min(r.x + r.width, shared.mode.width);
        const uint32_t bottom = std::min(r.y + r.height, shared.mode.height);
        if (r.x >= right || r.y >= bottom) {
            shared.hidden = RCS_CAPTURE_PAUSE_HIDDEN;
            return shared;
        }
        shared.region = {r.x, r.y, right - r.x, bottom - r.y};
    }
    if (target.source == RCS_CAPTURE_APPLICATION) {
        for (SCRunningApplication* application in content.applications) {
            if (application.processID == pid_t(target.processId)) {
                shared.filter = [[SCContentFilter alloc] initWithDisplay:shared.display
                                                   includingApplications:@[ application ]
                                                        exceptingWindows:@[]];
                return shared;
            }
        }
        shared.hidden = mac::processRuns(pid_t(target.processId)) ? RCS_CAPTURE_PAUSE_HIDDEN : RCS_CAPTURE_PAUSE_GONE;
        return shared;
    }
    NSMutableArray<SCRunningApplication*>* excluded = [NSMutableArray array];
    if (excludeCurrentProcess) {
        for (SCRunningApplication* application in content.applications) {
            if (application.processID == getpid()) {
                [excluded addObject:application];
            }
        }
    }
    shared.filter = [[SCContentFilter alloc] initWithDisplay:shared.display
                                       excludingApplications:excluded
                                            exceptingWindows:@[]];
    return shared;
}

class ScreenCaptureKitCapture final : public ScreenCapture {
  public:
    ~ScreenCaptureKitCapture() override { stop(); }

    void start(const CaptureConfig& config, CancelToken cancelled) override {
        if (screenPermission() != RCS_PERMISSION_GRANTED) {
            fail(RCS_ERROR_PERMISSION_REQUIRED, "Screen recording permission is required for this application");
        }
        config_ = config;
        target_ = config.target;
        window_ = target_.source == RCS_CAPTURE_WINDOW;
        display_ = window_ ? 0 : target_.displayId ? target_.displayId : CGMainDisplayID();
        cursorInVideo_ = config.cursorInVideo;
        cursor_ = std::make_unique<mac::CursorTracker>();
        pace(config.framesPerSecond);
        content_ = {0, 0, config.width, config.height}; // Until what is shared shows: the whole frame.
        // Starting needs a capturable display; only a running capture pauses.
        // A window or application that does not show yet is waited for.
        if (!shows() || !open(cancelled, false)) {
            if (hidden_ == RCS_CAPTURE_PAUSE_NONE) {
                fail(RCS_ERROR_CAPTURE, "The selected display cannot be captured; the screen may be locked or "
                                        "asleep, or the display is not connected");
            }
            pause(hidden_ == RCS_CAPTURE_PAUSE_GONE ? "what is shared is gone" : "what is shared does not show");
        }
        started_ = true;
    }

    bool next(VideoFrame& frame, std::chrono::milliseconds timeout, CancelToken cancelled) override {
        struct PollCursor { // After waiting, outside the lock.
            ScreenCaptureKitCapture& capture;
            ~PollCursor() {
                if (!capture.cursorInVideo_) {
                    capture.cursor_->poll(capture.area(), capture.display_ ? capture.display_ : CGMainDisplayID());
                }
            }
        } pollCursor{*this};
        if (!stream_) {
            return resume(timeout, cancelled);
        }
        if (const auto now = Clock::now(); now >= nextModeCheck_ || blankNow()) {
            nextModeCheck_ = now + retryInterval;
            if (!window_) {
                // A display turned is a mode change too: the stream starts again for its new shape.
                if (const auto now = mac::displayGeometry(display_); now.width) {
                    if (now.width != geometry_.width || now.height != geometry_.height ||
                        now.rotation != geometry_.rotation) {
                        pause("the display's mode or rotation changed");
                        return false;
                    }
                    geometry_.refreshHz = now.refreshHz; // The picture stays as it is.
                    mode_.refreshHz = now.refreshHz;
                }
            }
            if (!shows()) {
                if (hidden_ == RCS_CAPTURE_PAUSE_GONE || !window_) {
                    pause(hidden_ == RCS_CAPTURE_PAUSE_GONE ? "what is shared is gone"
                                                            : "the application shows no window on the display");
                    return false;
                }
                // A window that does not show keeps its stream: its frames are
                // suspended meanwhile, and come again with the window.
            }
        }
        std::unique_lock lock(state_->mutex);
        const auto wake = [&] {
            return state_->interrupted || state_->stopped || !state_->failure.empty() || cancelled.load();
        };
        state_->changed.wait_for(lock, timeout, [&] { return state_->hasLatest || wake(); });
        // Pace to the configured rate (due()). An early frame waits and may be
        // replaced by a newer one meanwhile.
        const auto slot = due();
        if (state_->hasLatest && !state_->interrupted && Clock::now() < slot) {
            state_->changed.wait_until(lock, slot, wake);
        }
        if (!state_->failure.empty()) {
            fail(RCS_ERROR_CAPTURE, state_->failure);
        }
        if (NSError* stopped = state_->stopped) {
            lock.unlock();
            failIfFinal(stopped);
            if (window_ && !shows()) { // The window closed: its stream ended with it.
                pause(hidden_ == RCS_CAPTURE_PAUSE_GONE ? "the window is gone" : "the window does not show");
            } else {
                pause("the stream stopped: " + text(stopped));
            }
            return false;
        }
        const bool early = Clock::now() < slot;
        state_->interrupted = false;
        if (!state_->hasLatest || early) {
            return false;
        }
        frame = std::move(state_->latest);
        frame.color = colorOf(config_.picture);
        state_->latest = {};
        state_->hasLatest = false;
        const CGRect content = state_->latestContent, screen = state_->latestScreen;
        lock.unlock();
        if (window_) {
            followWindow(content, screen);
        }
        took();
        return true;
    }

    void interrupt() noexcept override {
        std::lock_guard lock(state_->mutex);
        state_->interrupted = true;
        state_->changed.notify_one();
    }

    void stop() noexcept override {
        {
            std::lock_guard lock(state_->mutex);
            state_->closed = true;
            state_->latest = {};
            state_->hasLatest = false;
        }
        close();
    }

    uint64_t captured() const noexcept override {
        std::lock_guard lock(state_->mutex);
        return state_->frames;
    }
    uint64_t overwritten() const noexcept override {
        std::lock_guard lock(state_->mutex);
        return state_->overwritten;
    }
    // Pictures that come say it shows; otherwise what the window server says
    // of what is shared, and of the screen.
    rcs_capture_pause paused() const noexcept override {
        std::lock_guard lock(state_->mutex);
        const bool streaming = stream_ && !state_->blank;
        if (!started_ || state_->closed || (streaming && monotonicNs() - state_->lastFrameNs < 500'000'000)) {
            return RCS_CAPTURE_PAUSE_NONE;
        }
        if (const rcs_capture_pause hidden = hidden_; hidden != RCS_CAPTURE_PAUSE_NONE) {
            return hidden;
        }
        if (streaming) {
            return RCS_CAPTURE_PAUSE_NONE; // Still: nothing changes, nothing is hidden.
        }
        // A window's stream goes blank when it stops showing, a display's
        // when the screen locks, sleeps or changes its mode.
        return window_ && !locked_ ? RCS_CAPTURE_PAUSE_HIDDEN : RCS_CAPTURE_PAUSE_DESKTOP;
    }
    uint64_t interruptions() const noexcept override {
        std::lock_guard lock(state_->mutex);
        return state_->interruptions;
    }
    Rect content() const noexcept override { return content_; }
    DisplayMode display() const noexcept override { return mode_; }

    void setCursorInVideo(bool inVideo) override {
        if (inVideo == cursorInVideo_) {
            return;
        }
        cursorInVideo_ = inVideo;
        if (!stream_) {
            return; // Paused: the next stream starts with it.
        }
        settings_.showsCursor = inVideo;
        update();
    }
    CursorState cursor() const override { return cursor_ ? cursor_->state() : CursorState{}; }

  private:
    // Whether what is shared shows now; sets hidden_ to why not (GONE for good).
    bool shows() {
        if (hidden_ == RCS_CAPTURE_PAUSE_GONE) {
            return false;
        }
        bool shown = true, gone = false;
        if (window_) {
            const auto state = mac::windowState(CGWindowID(target_.windowId));
            gone = !state;
            shown = state && state->onScreen;
            if (shown) {
                windowBounds_ = state->bounds;
            }
        } else if (target_.source == RCS_CAPTURE_APPLICATION) {
            gone = !mac::processRuns(pid_t(target_.processId));
            shown = !gone && mac::showsOn(pid_t(target_.processId), display_);
        }
        hidden_ = gone ? RCS_CAPTURE_PAUSE_GONE : shown ? RCS_CAPTURE_PAUSE_NONE : RCS_CAPTURE_PAUSE_HIDDEN;
        if (window_) {
            locked_ = mac::screenLocked();
        }
        return shown;
    }

    // The stream went blank since the last look: a window's frames are
    // suspended when it stops showing, which is looked at once.
    bool blankNow() {
        std::lock_guard lock(state_->mutex);
        const bool blank = state_->blank && !blankSeen_;
        blankSeen_ = state_->blank;
        return blank;
    }

    // Where the pointer is reported across: what is shared, in global points.
    CGRect area() const {
        if (window_) {
            return windowBounds_;
        }
        const CGRect bounds = CGDisplayBounds(display_);
        if (target_.source != RCS_CAPTURE_REGION) {
            return bounds;
        }
        return CGRectMake(bounds.origin.x + region_.origin.x, bounds.origin.y + region_.origin.y, region_.size.width,
                          region_.size.height);
    }

    // A window's frame says where it sits in the frame and on screen; a new
    // size moves its place in the stream so that it stays fitted. Where it
    // sits is the place the stream was given, unless the frame says
    // otherwise by more than its rounding: a frame that came before the
    // stream took the new place.
    void followWindow(CGRect content, CGRect screen) {
        if (!CGRectIsNull(content) && content.size.width > 0) {
            const Rect said = rounded(content);
            const auto near = [](uint32_t a, uint32_t b) { return (a > b ? a - b : b - a) <= 4; };
            content_ = near(said.x, placed_.x) && near(said.y, placed_.y) && near(said.width, placed_.width) &&
                               near(said.height, placed_.height)
                           ? placed_
                           : said;
        }
        if (CGRectIsNull(screen) || screen.size.width < 1 || screen.size.height < 1) {
            return;
        }
        windowBounds_ = screen;
        if (std::fabs(screen.size.width - windowSize_.width) < 0.5 &&
            std::fabs(screen.size.height - windowSize_.height) < 0.5) {
            return;
        }
        windowSize_ = screen.size;
        if (const CGDirectDisplayID shown = mac::displayShowing(screen)) {
            display_ = shown;
        }
        place(screen.size, mac::pixelsPerPoint(display_ ? display_ : CGMainDisplayID()));
        update();
    }

    // Fits a window of `size` points at `scale` into the frame.
    void place(CGSize size, double scale) {
        const auto width = uint32_t(std::lround(size.width * scale)),
                   height = uint32_t(std::lround(size.height * scale));
        const auto geometry = viewGeometry(int(config_.width), int(config_.height), width, height);
        settings_.destinationRect = CGRectMake(geometry.x, geometry.y, geometry.width, geometry.height);
        placed_ = {uint32_t(geometry.x), uint32_t(geometry.y), uint32_t(geometry.width), uint32_t(geometry.height)};
        mode_ = {width, height, display_ ? mac::displayGeometry(display_).refreshHz : 0};
    }

    // Hands changed settings to the running stream.
    void update() {
        const auto state = state_;
        [stream_ updateConfiguration:settings_
                   completionHandler:^(NSError* error) {
                     if (error) {
                         std::lock_guard lock(state->mutex);
                         state->failure = "Changing the capture's settings failed: " + text(error);
                         state->changed.notify_all();
                     }
                   }];
    }

    // Starts a stream on what is shared as it is now; false while it cannot
    // be (the display not listed: locked, asleep, being reconfigured; the
    // window or application not shareable now, hidden_ says why). Resuming,
    // a listing the system refuses counts as that too; starting, it fails.
    bool open(CancelToken cancelled, bool resuming) {
        const auto listed = listContent(window_, cancelled);
        if (listed.error && resuming) {
            return false;
        }
        throwIfError(listed.error, "Listing shareable content");
        SCStreamConfiguration* settings = [SCStreamConfiguration new];
        settings_ = settings;
        SCContentFilter* filter = window_ ? windowFilter(listed.content) : displayFilter(listed.content);
        if (!filter) {
            settings_ = nil;
            return false;
        }
        settings.width = config_.width;
        settings.height = config_.height;
        settings.scalesToFit = YES;
        // At exactly the frame interval, ScreenCaptureKit skips a display refresh
        // whenever a frame comes slightly early (54 instead of 60 FPS); next()
        // paces instead, so frames may arrive twice as often.
        settings.minimumFrameInterval = CMTimeMake(1, int32_t(config_.framesPerSecond * 2));
        // The encoder's own layout and matrix: ScreenCaptureKit converts.
        settings.pixelFormat = apple::pixelFormatOf(config_.picture);
        settings.colorMatrix = apple::colorKeysOf(config_.picture).matrix;
        // Surfaces in flight: up to three in the encoder, the latest-frame slot,
        // and the one being written. Fewer stalls capture at 4K120.
        settings.queueDepth = 6;
        settings.showsCursor = cursorInVideo_;
        settings.colorSpaceName = kCGColorSpaceSRGB;

        if (!sink_) {
            sink_ = [[RCSCaptureSink alloc] initWithState:state_];
            queue_ = dispatch_queue_create("rcsdk.screen-capture", DISPATCH_QUEUE_SERIAL);
        }
        stream_ = [[SCStream alloc] initWithFilter:filter configuration:settings delegate:sink_];
        {
            std::lock_guard lock(state_->mutex);
            state_->stream = (__bridge const void*)stream_;
            state_->stopped = nil;
            state_->blank = false;
        }
        blankSeen_ = false;
        NSError* attachError = nil;
        if (![stream_ addStreamOutput:sink_
                                 type:SCStreamOutputTypeScreen
                   sampleHandlerQueue:queue_
                                error:&attachError]) {
            throwIfError(attachError, "Attaching capture output");
            fail(RCS_ERROR_CAPTURE, "Attaching capture output failed");
        }
        __block NSError* startError = nil;
        dispatch_semaphore_t started = dispatch_semaphore_create(0);
        [stream_ startCaptureWithCompletionHandler:^(NSError* failure) {
          startError = failure;
          dispatch_semaphore_signal(started);
        }];
        await(started, cancelled, "Starting screen capture");
        throwIfError(startError, "Starting screen capture");
        nextModeCheck_ = Clock::now() + retryInterval;
        return true;
    }

    // DISPLAY, REGION and APPLICATION: the display as listed, and what of it
    // is shared. Null while it is not listed, or (REGION) the region is off it.
    SCContentFilter* displayFilter(SCShareableContent* content) {
        const auto shared = shareable(content, target_, display_, config_.excludeCurrentProcess);
        if (!shared.display) {
            return nil;
        }
        geometry_ = mac::displayGeometry(display_); // To see its mode or its rotation change.
        mode_ = shared.mode;
        SCDisplay* display = shared.display;
        Rect area{0, 0, uint32_t(display.width), uint32_t(display.height)}; // Points: only its aspect matters.
        if (target_.source == RCS_CAPTURE_REGION) {
            if (shared.hidden != RCS_CAPTURE_PAUSE_NONE) { // None of it on the display.
                hidden_ = shared.hidden;
                return nil;
            }
            // The region in the display's pixels as it is now, as points for the stream.
            const auto& r = shared.region;
            const double scale = shared.mode.width ? double(shared.mode.width) / display.width : 1;
            region_ = CGRectMake(r.x / scale, r.y / scale, r.width / scale, r.height / scale);
            settings_.sourceRect = region_;
            area = {0, 0, r.width, r.height};
            mode_ = {r.width, r.height, shared.mode.refreshHz};
        }
        const auto geometry = viewGeometry(int(config_.width), int(config_.height), area.width, area.height);
        content_ = {uint32_t(geometry.x), uint32_t(geometry.y), uint32_t(geometry.width), uint32_t(geometry.height)};
        settings_.destinationRect = CGRectMake(geometry.x, geometry.y, geometry.width, geometry.height);
        if (!shared.filter) { // APPLICATION: none of it listed.
            hidden_ = shared.hidden;
        }
        return shared.filter;
    }

    // WINDOW: the window as listed; null while it is not (hidden_ says why).
    SCContentFilter* windowFilter(SCShareableContent* content) {
        const auto shared = shareable(content, target_, 0, false);
        if (!shared.filter) {
            hidden_ = shared.hidden;
            return nil;
        }
        const CGRect frame = shared.window.frame;
        windowBounds_ = frame;
        windowSize_ = frame.size;
        display_ = mac::displayShowing(frame);
        place(frame.size, shared.filter.pointPixelScale);
        content_ = placed_;
        settings_.ignoreShadowsSingleWindow = YES;
        return shared.filter;
    }

    // Ends the stream, if any; its late output no longer counts.
    void close() noexcept {
        if (stream_) {
            {
                std::lock_guard lock(state_->mutex);
                state_->stream = nullptr;
            }
            dispatch_semaphore_t stopped = dispatch_semaphore_create(0);
            [stream_ stopCaptureWithCompletionHandler:^(NSError*) {
              dispatch_semaphore_signal(stopped);
            }];
            dispatch_semaphore_wait(stopped, dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC));
            [stream_ removeStreamOutput:sink_ type:SCStreamOutputTypeScreen error:nil];
            dispatch_sync(queue_, ^{
                          }); // Drain callbacks already queued.
        }
        stream_ = nil;
        settings_ = nil;
    }

    void pause(const std::string& why) {
        log::info("capture", "Screen capture paused: " + why);
        close();
        std::lock_guard lock(state_->mutex);
        state_->latest = {};
        state_->hasLatest = false;
        ++state_->interruptions;
        nextAttempt_ = {}; // Try at once: a mode change is usually over.
    }

    // While paused, tries to start a stream every 250 ms; false (after
    // waiting up to `timeout`) while what is shared cannot be captured, and
    // false once more when it resumed: the next call waits for its picture.
    bool resume(std::chrono::milliseconds timeout, CancelToken cancelled) {
        const auto now = Clock::now();
        if (now < nextAttempt_ || hidden_ == RCS_CAPTURE_PAUSE_GONE) {
            std::unique_lock lock(state_->mutex);
            const auto wait = hidden_ == RCS_CAPTURE_PAUSE_GONE
                                  ? std::chrono::nanoseconds(timeout)
                                  : std::min<std::chrono::nanoseconds>(nextAttempt_ - now, timeout);
            state_->changed.wait_for(lock, wait, [&] { return state_->interrupted || cancelled.load(); });
            state_->interrupted = false;
            return false;
        }
        nextAttempt_ = now + retryInterval;
        if (screenPermission() != RCS_PERMISSION_GRANTED) {
            fail(RCS_ERROR_PERMISSION_REQUIRED, "Screen recording permission was withdrawn");
        }
        if (!window_) {
            if (!online(display_)) {
                offlineSince_ = offlineSince_.value_or(now);
                if (now - *offlineSince_ >= disconnectedLimit) {
                    fail(RCS_ERROR_CAPTURE, "The shared display was disconnected");
                }
                return false;
            }
            offlineSince_.reset();
        }
        if (!shows()) {
            return false; // A window or application that does not show is looked at, not listed.
        }
        try {
            if (!open(cancelled, true)) {
                if (window_ || target_.source == RCS_CAPTURE_APPLICATION) {
                    // Shows but is not shareable (a window that keeps itself
                    // out of captures): listing again is slow, so less often.
                    nextAttempt_ = now + 1s;
                }
                return false; // Locked, asleep or reconfiguring: later.
            }
        } catch (const Failure& failure) { // It is listed, its stream does not start.
            close();
            if (failure.result() == RCS_ERROR_CANCELLED || ++refused_ >= refusedStarts) {
                throw;
            }
            return false;
        }
        refused_ = 0;
        log::info("capture", "Screen capture resumed at " + std::to_string(mode_.width) + "x" +
                                 std::to_string(mode_.height));
        return false;
    }

    std::shared_ptr<mac::CaptureState> state_ = std::make_shared<mac::CaptureState>();
    bool started_ = false;
    CaptureConfig config_;
    CaptureTarget target_;
    bool window_ = false;
    // Why what is shared does not show (NONE while it does); read by
    // paused() on any thread.
    std::atomic<rcs_capture_pause> hidden_{RCS_CAPTURE_PAUSE_NONE};
    std::atomic<bool> locked_{false}; // WINDOW: the screen is locked (as last looked at).
    // The display captured, or (WINDOW) the one showing most of the window.
    CGDirectDisplayID display_ = 0;
    mac::DisplayGeometry geometry_; // The display's own, to see it change.
    CGRect region_{};         // REGION: in the display's points.
    CGRect windowBounds_{};   // WINDOW: on screen, global points...
    CGSize windowSize_{};     // ...and the size the stream places it for.
    Rect content_;
    Rect placed_; // WINDOW: where the stream was told to place the window.
    DisplayMode mode_;
    bool cursorInVideo_ = true;
    bool blankSeen_ = false;
    std::unique_ptr<mac::CursorTracker> cursor_;
    SCStreamConfiguration* settings_ = nil;
    Clock::time_point nextModeCheck_{};
    Clock::time_point nextAttempt_{};
    std::optional<Clock::time_point> offlineSince_;
    int refused_ = 0;
    RCSCaptureSink* sink_ = nil;
    SCStream* stream_ = nil;
    dispatch_queue_t queue_ = nil;
};

} // namespace

std::unique_ptr<ScreenCapture> createScreenCapture(const CaptureTarget&) {
    return std::make_unique<ScreenCaptureKitCapture>();
}

// One picture through SCScreenshotManager with the filter a capture of the
// same target uses. What a running capture would pause for is looked at
// first: ScreenCaptureKit does not say what it makes of a window that does
// not show.
Thumbnail captureThumbnail(const ThumbnailRequest& request) {
    static const std::atomic<bool> never{false};
    const auto deadline = Clock::now() + 3s;
    const auto& target = request.target;
    if (screenPermission() != RCS_PERMISSION_GRANTED) {
        fail(RCS_ERROR_PERMISSION_REQUIRED, "Screen recording permission is required for this application");
    }
    if (mac::screenLocked()) {
        fail(RCS_ERROR_INVALID_STATE, "The screen is locked");
    }
    const bool window = target.source == RCS_CAPTURE_WINDOW;
    const CGDirectDisplayID display = window ? 0 : target.displayId ? target.displayId : CGMainDisplayID();
    const auto process = pid_t(target.processId);
    if (window) {
        const auto state = mac::windowState(CGWindowID(target.windowId));
        if (!state) {
            fail(RCS_ERROR_INVALID_STATE, "The window is gone");
        }
        if (!state->onScreen) {
            fail(RCS_ERROR_INVALID_STATE, "The window does not show: it is minimized, hidden or on another Space");
        }
    } else if (CGDisplayIsAsleep(display)) {
        fail(RCS_ERROR_INVALID_STATE, "The display is asleep");
    } else if (target.source == RCS_CAPTURE_APPLICATION &&
               (!mac::processRuns(process) || !mac::showsOn(process, display))) {
        fail(RCS_ERROR_INVALID_STATE,
             mac::processRuns(process) ? "The application shows no window on the display" : "The application quit");
    }
    @autoreleasepool {
        const auto listed = listContent(window, never, deadline, RCS_ERROR_TIMEOUT);
        failIfFinal(listed.error);
        throwIfError(listed.error, "Listing shareable content");
        const auto shared = shareable(listed.content, target, display, request.excludeCurrentProcess);
        if (!shared.filter) {
            const bool gone = shared.hidden == RCS_CAPTURE_PAUSE_GONE;
            fail(RCS_ERROR_INVALID_STATE, shared.hidden == RCS_CAPTURE_PAUSE_NONE
                                              ? "The display cannot be captured now: it may be asleep or being "
                                                "reconfigured"
                                          : window ? (gone ? "The window is gone" : "The window does not show")
                                          : target.source == RCS_CAPTURE_REGION ? "The region is off the display"
                                          : gone ? "The application quit"
                                                 : "The application shows no window on the display");
        }
        // What is shared in pixels, scaled into the picture: a region of the
        // display (in its points for the configuration), the display, or the
        // window at its display's scale.
        SCStreamConfiguration* settings = [SCStreamConfiguration new];
        uint32_t width = shared.mode.width, height = shared.mode.height;
        if (target.source == RCS_CAPTURE_REGION) {
            const auto& r = shared.region;
            const double scale = double(shared.mode.width) / shared.display.width;
            settings.sourceRect = CGRectMake(r.x / scale, r.y / scale, r.width / scale, r.height / scale);
            width = r.width;
            height = r.height;
        } else if (window) {
            const CGSize size = shared.window.frame.size;
            width = uint32_t(std::lround(size.width * shared.filter.pointPixelScale));
            height = uint32_t(std::lround(size.height * shared.filter.pointPixelScale));
            settings.ignoreShadowsSingleWindow = YES;
        }
        if (!width || !height) {
            fail(RCS_ERROR_INVALID_STATE, "What is shared has no size now");
        }
        const auto [pictureWidth, pictureHeight] = thumbnailSize(width, height, request.maxWidth, request.maxHeight);
        settings.width = pictureWidth;
        settings.height = pictureHeight;
        settings.scalesToFit = YES;
        settings.preservesAspectRatio = NO; // The size has the aspect already, rounded.
        settings.showsCursor = NO;
        settings.pixelFormat = kCVPixelFormatType_32BGRA;
        settings.colorSpaceName = kCGColorSpaceSRGB;

        __block CVPixelBufferRef buffer = nullptr;
        __block NSError* error = nil;
        dispatch_semaphore_t taken = dispatch_semaphore_create(0);
        [SCScreenshotManager
            captureSampleBufferWithFilter:shared.filter
                            configuration:settings
                        completionHandler:^(CMSampleBufferRef sample, NSError* failure) {
                            if (CVPixelBufferRef image = sample ? CMSampleBufferGetImageBuffer(sample) : nullptr) {
                                buffer = CVPixelBufferRetain(image);
                            }
                            error = failure;
                            dispatch_semaphore_signal(taken);
                        }];
        try {
            await(taken, never, "Taking the picture", deadline, RCS_ERROR_TIMEOUT);
        } catch (...) {
            // The handler still comes, and must find what it writes to.
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                dispatch_semaphore_wait(taken, DISPATCH_TIME_FOREVER);
                if (buffer) {
                    CVPixelBufferRelease(buffer);
                }
            });
            throw;
        }
        const auto release = std::shared_ptr<void>(buffer, [](void* image) {
            if (image) {
                CVPixelBufferRelease(static_cast<CVPixelBufferRef>(image));
            }
        });
        failIfFinal(error);
        throwIfError(error, "Taking the picture");
        if (!buffer || CVPixelBufferGetPixelFormatType(buffer) != kCVPixelFormatType_32BGRA ||
            CVPixelBufferGetWidth(buffer) != pictureWidth || CVPixelBufferGetHeight(buffer) != pictureHeight) {
            fail(RCS_ERROR_CAPTURE, "ScreenCaptureKit delivered no picture of the size asked for");
        }
        if (CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
            fail(RCS_ERROR_CAPTURE, "Could not map the picture");
        }
        Thumbnail picture{pictureWidth, pictureHeight, std::vector<uint8_t>(size_t(pictureWidth) * pictureHeight * 4)};
        const auto* rows = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
        const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
        for (uint32_t y = 0; y < pictureHeight; ++y) {
            uint8_t* to = picture.pixels.data() + size_t(y) * pictureWidth * 4;
            std::memcpy(to, rows + y * stride, size_t(pictureWidth) * 4);
            for (uint32_t x = 0; x < pictureWidth; ++x) {
                to[x * 4 + 3] = 255; // What no window covers (an application's) comes transparent.
            }
        }
        CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        return picture;
    }
}

} // namespace rcsdk::platform
