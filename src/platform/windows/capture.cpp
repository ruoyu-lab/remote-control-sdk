// Screen capture with DXGI Desktop Duplication: a display, a region of one, or
// an application's windows on one (single windows: window_capture.cpp). Each
// frame is scaled, converted from HDR when needed and composited with the
// cursor on the GPU, straight from the duplicated desktop into a picture
// texture of a small pool (unless the cursor goes separately: then pointer
// moves alone yield no frame and cursor() reports the pointer, its image
// converted to BGRA). When the desktop switches (lock screen, a UAC prompt on
// the secure desktop) or its mode changes, duplication is lost; capture pauses
// and duplicates the output again once the desktop can be captured. A display
// that is disconnected pauses it too, and its output is found again when it
// comes back (on the same GPU); one disconnected for 10 s fails capture, as on
// macOS.
//
// A region is the part of the desktop texture drawn; after a mode change it
// is clipped to the display, and none of it left pauses capture. An
// application shows where its windows are in front, black elsewhere: at each
// desktop update the parts of the display where its windows show are worked
// out from the windows in front, and only those are drawn. No window of it on
// the display pauses capture; its process ending pauses it for good.
//
// exclude_current_process keeps every top-level window of this process out of
// the picture with WDA_EXCLUDEFROMCAPTURE, also windows the host opens while
// sharing (a notification, a dialog, a menu). The affinity is per window, so
// new windows are looked for at every desktop update; a picture that may
// already show one is withheld until the desktop has updated without it.
//
// Thumbnails (rcs_capture_picture) of a display: Windows lets a process
// duplicate each display once at a time (a second DuplicateOutput is
// E_INVALIDARG), so a capture of this process that runs on the display takes
// them, on its own thread (the duplication belongs to the desktop that thread
// is on), from the next desktop image, or from the whole desktop a new
// duplication shows at once when the desktop is still; the image stays held
// for the capture's own next frame. A display no capture of this process
// duplicates is duplicated for the one picture, and a capture that starts on
// it meanwhile waits for that.
//
// A turned display (portrait, flipped) is captured upright. What Microsoft
// documents ("Desktop Duplication API", its sample, and as WebRTC uses it):
// the duplicated texture is the panel's image, unrotated, the desktop turned
// within it; DXGI_OUTDUPL_DESC.Rotation says how; its ModeDesc, the
// display's place, the pointer's position and shape are the desktop's, as
// shown (dirty and move rectangles are the texture's; unused here). So all
// of this works in the desktop's orientation, a region, an application's
// windows, the pointer and remote input as on any display, and only the draw
// samples the texture turned (surface_shader.hpp). Turning the display is a
// mode change: duplication is lost, and capture resumes with the new shape.
#include "platform/windows/gdi_cursor.hpp"
#include "platform/windows/d3d.hpp"
#include "platform/windows/desktop.hpp"
#include "platform/windows/display.hpp"
#include "platform/windows/picture_pool.hpp"
#include "platform/windows/surface_shader.hpp"
#include "platform/windows/thumbnails.hpp"
#include "platform/windows/win.hpp"
#include "platform/windows/window.hpp"
#include "platform/windows/window_capture.hpp"

#include "platform/platform.hpp"
#include "util/log.hpp"

#include <rcsdk/view_geometry.hpp>

#include <dxgi1_5.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace rcsdk::platform {
namespace {

using win::Com;

constexpr rcs_result captureError = RCS_ERROR_CAPTURE;
constexpr auto disconnectedLimit = 10s; // A display disconnected that long is gone.

// Duplication is impossible right now but will work again: the secure desktop
// or lock screen holds the display (only LOCAL_SYSTEM may capture them), the
// session is disconnected, a mode change is under way, or the desktop is in a
// mode duplication does not copy (DuplicateOutput's documentation: try again
// after a desktop switch or a mode change).
bool transient(HRESULT result) {
    return result == E_ACCESSDENIED || result == DXGI_ERROR_ACCESS_LOST || result == DXGI_ERROR_SESSION_DISCONNECTED ||
           result == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE || result == DXGI_ERROR_MODE_CHANGE_IN_PROGRESS ||
           result == DXGI_ERROR_UNSUPPORTED;
}

std::string hex(HRESULT result) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(result));
    return text;
}

// Why DuplicateOutput is E_INVALIDARG (outside a Remote Desktop session): a
// second capture of the display in this process, or the host's own.
constexpr const char* duplicatedElsewhere =
    "another part of this process duplicates the display (Windows allows a process one duplication of a display at "
    "a time)";

// A duplication that failed for good. In a Remote Desktop session the failure
// says so: the SDK captures one with desktop duplication only, and on Windows
// 10 duplicating it again after its client reconnected has been reported to
// fail with E_INVALIDARG (Microsoft Q&A 1068807). A capture that pauses does
// not change device; one started anew (a new run, an agent that connects
// again) opens the display afresh.
void checkDuplication(HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) {
        return;
    }
    if (const auto session = win::remoteSession()) {
        fail(captureError, std::string(operation) + " failed in Remote Desktop session " + std::to_string(*session) +
                               " (HRESULT " + hex(result) +
                               "); the SDK captures it with desktop duplication only. After the client reconnects, "
                               "Windows 10 is reported to refuse it: start the capture anew");
    }
    if (result == E_INVALIDARG) {
        fail(captureError, "Desktop duplication failed (HRESULT " + hex(result) + "): " + duplicatedElsewhere);
    }
    win::check(result, operation, captureError);
}

// The QueryPerformanceCounter value `delay` from now, the clock DXGI stamps
// desktop updates with.
int64_t performanceCounterIn(std::chrono::milliseconds delay) {
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return now.QuadPart + frequency.QuadPart * delay.count() / 1000;
}

// A DXGI pointer shape as straight-alpha BGRA. Pixels that invert the screen
// beneath (monochrome AND 1 XOR 1, masked-color XOR pixels) have no straight
// equivalent: they become opaque black, or the colour they XOR with.
std::vector<uint8_t> straightPixels(const std::vector<uint8_t>& shape, const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info,
                                    UINT width, UINT height) {
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            uint8_t* p = pixels.data() + (size_t(y) * width + x) * 4;
            if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                const uint8_t bit = uint8_t(0x80 >> (x % 8));
                const bool andBit = shape[size_t(y) * info.Pitch + x / 8] & bit;
                const bool xorBit = shape[size_t(y + height) * info.Pitch + x / 8] & bit;
                const uint8_t value = !andBit && xorBit ? 255 : 0;
                p[0] = p[1] = p[2] = value;
                p[3] = andBit && !xorBit ? 0 : 255;
                continue;
            }
            std::memcpy(p, shape.data() + size_t(y) * info.Pitch + x * 4, 4);
            if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
                p[3] = p[3] == 0 || (p[0] | p[1] | p[2]) ? 255 : 0; // Mask 0: opaque; XOR with 0: transparent.
            }
        }
    }
    return pixels;
}

// On an HDR desktop, the scRGB value of SDR white on the display `device`
// (\\.\DISPLAYN) names: its "SDR content brightness", where 1 is 80 nits.
// None when Windows does not say.
std::optional<float> sdrWhite(const std::wstring& device) {
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }
    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || device != source.viewGdiDeviceName) {
            continue;
        }
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel) {
            return float(white.SDRWhiteLevel) / 1000; // SDRWhiteLevel / 1000 * 80 nits.
        }
    }
    return std::nullopt;
}

// DuplicateOutput's refusals, as a thumbnail reports them: INVALID_STATE where
// the display cannot be duplicated now, CAPTURE where it cannot be at all.
[[noreturn]] void failDuplication(HRESULT result) {
    const char* why = nullptr;
    switch (result) {
    case E_INVALIDARG:
        why = duplicatedElsewhere;
        break;
    case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:
        why = "four processes duplicate the desktop already (Windows' limit), or the display is being reconfigured";
        break;
    case E_ACCESSDENIED:
        why = "a secure desktop (the sign-in or lock screen, a UAC prompt) is in front";
        break;
    case DXGI_ERROR_SESSION_DISCONNECTED:
        why = "the Windows session is disconnected";
        break;
    case DXGI_ERROR_MODE_CHANGE_IN_PROGRESS:
    case DXGI_ERROR_ACCESS_LOST:
        why = "the desktop changed (a secure desktop, a mode change)";
        break;
    default:
        win::check(result, "Desktop duplication", captureError);
        fail(captureError, "Desktop duplication failed");
    }
    fail(RCS_ERROR_INVALID_STATE, "The display cannot be captured now (HRESULT " + hex(result) + "): " + why);
}

// A thumbnail another thread asks a running capture for, and its answer.
struct ThumbnailJob {
    ThumbnailRequest request;
    Clock::time_point deadline;
    std::mutex mutex;
    std::condition_variable answered;
    std::optional<Thumbnail> picture;
    std::exception_ptr failure;
    bool done = false;

    void answer(std::optional<Thumbnail> taken, std::exception_ptr failed) {
        {
            std::lock_guard lock(mutex);
            picture = std::move(taken);
            failure = std::move(failed);
            done = true;
        }
        answered.notify_all();
    }
};

class DesktopDuplicationCapture;

// The displays this process duplicates, by id (see the top of the file).
struct Duplications {
    std::mutex mutex;
    std::condition_variable changed;
    std::map<uint32_t, DesktopDuplicationCapture*> running;
    std::set<uint32_t> pictures; // Displays a thumbnail duplicates for itself now...
    std::mutex oneAtATime;       // ...one such thumbnail at a time.
};
Duplications& duplications() {
    static auto* value = new Duplications; // Captures may still run while the process exits.
    return *value;
}

class DesktopDuplicationCapture final : public ScreenCapture {
public:
    // `picture`: for one thumbnail (desktopThumbnail), not a stream.
    explicit DesktopDuplicationCapture(bool picture = false) : picture_(picture) {}
    ~DesktopDuplicationCapture() override { stop(); }

    void start(const CaptureConfig& config, CancelToken cancelled) override {
        config_ = config;
        cursorInVideo_ = config.cursorInVideo;
        pace(config.framesPerSecond);
        if (config.secureDesktop && !desktop_.attach()) {
            fail(RCS_ERROR_PERMISSION_REQUIRED,
                 "Capture cannot follow the desktop that receives input (Windows error " +
                     std::to_string(desktop_.error()) +
                     "); secure_desktop needs a process running as LocalSystem in a user session");
        }
        if (config.excludeCurrentProcess) {
            hideOwnWindows();
        }
        const auto monitor = win::findMonitor(config.target.displayId); // 0: the main display now, kept.
        if (!monitor) {
            fail(captureError, "The selected display is not available");
        }
        if (config.target.source == RCS_CAPTURE_APPLICATION) {
            // Held, the handle keeps the process's number from being used again.
            process_ = OpenProcess(SYNCHRONIZE, FALSE, config.target.processId);
            if (!process_) {
                absent_ = RCS_CAPTURE_PAUSE_GONE; // Gone before capture started: it starts paused, for good.
            }
        }
        device_ = monitor->device;
        displayId_ = monitor->id;
        if (!picture_) {
            attach(cancelled);
        }
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(device_.c_str(), ENUM_CURRENT_SETTINGS, &mode)) {
            display_.refreshHz = mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0;
        }
        Com<IDXGIAdapter1> adapter;
        Com<IDXGIOutput> output;
        if (!findOutput(monitor->handle, adapter, output)) {
            fail(captureError, "No DXGI output drives the selected display");
        }
        monitor_ = monitor->handle;
        open(adapter.value, output.value);
    }

    bool next(VideoFrame& frame, std::chrono::milliseconds timeout, CancelToken cancelled) override {
        win::trace("capture next");
        serveThumbnails();
        if (absent_ == RCS_CAPTURE_PAUSE_GONE) {
            std::this_thread::sleep_for(timeout); // Gone for good: nothing to wait for.
            return false;
        }
        if (!duplication_ && !resume(timeout)) {
            return false;
        }
        if (hdr_ && Clock::now() >= nextWhiteRead_) { // Before the pacing wait, not in the frame's path.
            readSdrWhite();
        }
        // Paced to the configured rate (due()), then the next desktop image.
        const auto target = due();
        for (auto left = target - Clock::now(); left.count() > 0 && !cancelled; left = target - Clock::now()) {
            sleepPrecise(std::min<std::chrono::nanoseconds>(left, 4ms));
        }
        throwIfCancelled(cancelled);
        win::trace("capture paced");
        // The next desktop image: one a thumbnail took and held, or the next that comes.
        DXGI_OUTDUPL_FRAME_INFO info{};
        Com<IDXGIResource> resource;
        HRESULT acquired = S_OK;
        if (held_) {
            held_ = false;
            info = heldInfo_;
            resource = std::move(heldResource_);
        } else {
            acquired = duplication_->AcquireNextFrame(UINT(timeout.count()), &info, resource.out());
        }
        if (acquired == DXGI_ERROR_WAIT_TIMEOUT) {
            win::trace("capture timeout");
            // An application that quit may leave the desktop as it was: no
            // update says so.
            if (process_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
                log::info("capture", "Capture paused: the shared application quit");
                absent_ = RCS_CAPTURE_PAUSE_GONE;
            }
            return false;
        }
        // Following secure desktops, a switch of the input desktop can also
        // show as an invalid call (the duplication belongs to the desktop this
        // thread was on): the same pause, and the thread follows in resume().
        if (acquired == DXGI_ERROR_ACCESS_LOST ||
            (config_.secureDesktop && (acquired == DXGI_ERROR_INVALID_CALL || transient(acquired)))) {
            win::trace("capture paused");
            if (config_.secureDesktop) {
                log::info("capture",
                          "Desktop duplication was lost (HRESULT " + hex(acquired) + "): the input desktop changed");
            }
            duplication_.reset();
            ++interruptions_;
            return false;
        }
        win::check(acquired, "Desktop acquisition", captureError);
        win::trace("capture acquired");
        struct Release { // Holds a reference: the duplication may be dropped meanwhile.
            Com<IDXGIOutputDuplication> duplication;
            ~Release() { duplication->ReleaseFrame(); }
        } release;
        release.duplication.assign(duplication_.value);

        took();
        if (info.AccumulatedFrames > 1) {
            overwritten_ += info.AccumulatedFrames - 1;
        }
        updateCursor(info);
        if (!cursorInVideo_ && !info.LastPresentTime.QuadPart) {
            return false; // Only the pointer changed; cursor() has it.
        }
        if (config_.excludeCurrentProcess && info.LastPresentTime.QuadPart && hideOwnWindows()) {
            // This picture may show the window just hidden, and so may updates
            // the compositor started before the change reached it.
            withheldUntil_ = performanceCounterIn(50ms);
        }
        if (withheldUntil_) {
            if (info.LastPresentTime.QuadPart < withheldUntil_) {
                return false; // Pointer-only updates (0) still show the old desktop image.
            }
            withheldUntil_ = 0;
        }
        if (config_.target.source == RCS_CAPTURE_APPLICATION && (info.LastPresentTime.QuadPart || !visibleKnown_) &&
            !followApplication()) {
            return false;
        }
        if (absent_ != RCS_CAPTURE_PAUSE_NONE) {
            return false; // A region off the display.
        }

        // The shader reads the duplicated desktop itself, while the frame is
        // held (ReleaseFrame comes after the draw is queued).
        auto source = resource.as<ID3D11Texture2D>("Desktop texture", captureError);
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        if (!panelShaped(desc)) {
            // A mode change under way whose loss has not come yet: duplicated
            // again, as when it is lost.
            duplication_.reset();
            ++interruptions_;
            return false;
        }
        if (source.value != viewed_) { // Duplication hands out the same texture until it is lost.
            win::check(d3d_.device->CreateShaderResourceView(source.value, nullptr, sourceView_.out()),
                       "Desktop texture view", captureError);
            viewed_ = source.value; // Alive while sourceView_ holds it.
        }
        auto output = pictures_.next(d3d_.device.value, config_.width, config_.height);
        win::SurfaceDraw draw;
        draw.source = sourceView_.value;
        draw.sourceWidth = desc.Width;
        draw.sourceHeight = desc.Height;
        draw.turns = turns_;
        draw.from = from_;
        draw.sdrWhite = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? white_ : 0;
        draw.target = output.target.value;
        draw.to = {LONG(content_.x), LONG(content_.y), LONG(content_.x + content_.width),
                   LONG(content_.y + content_.height)};
        draw.visible = config_.target.source == RCS_CAPTURE_APPLICATION ? &visible_ : nullptr;
        draw.pointer = cursorVisible_ && cursorInVideo_ ? cursorView_.value : nullptr;
        draw.pointerType = cursorShape_.Type;
        draw.pointerRect = {cursorPoint_.x, cursorPoint_.y, cursorPoint_.x + LONG(cursorWidth_),
                            cursorPoint_.y + LONG(cursorHeight_)};
        shader_.draw(d3d_.device.value, d3d_.context.value, draw);

        frame.width = config_.width;
        frame.height = config_.height;
        frame.sequence = ++frames_;
        frame.timestampNs = monotonicNs();
        frame.surface = std::move(output.surface);
        frame.device = win::share(d3d_.device.value);
        frame.waitGpu = win::gpuFence(d3d_.device.value, d3d_.context.value);
        win::trace("capture drawn");
        return true;
    }

    void stop() noexcept override {
        leave();
        held_ = false;
        heldResource_.reset();
        duplication_.reset();
        output_.reset();
        factory_.reset();
        sourceView_.reset();
        viewed_ = nullptr;
        pictures_.clear();
        if (process_) {
            CloseHandle(process_);
            process_ = nullptr;
        }
        cursorView_.reset();
        d3d_ = {};
        for (const HWND window : hidden_) {
            SetWindowDisplayAffinity(window, WDA_NONE);
        }
        hidden_.clear();
        desktop_.restore();
    }

    uint64_t captured() const noexcept override { return frames_; }
    uint64_t overwritten() const noexcept override { return overwritten_; }
    rcs_capture_pause paused() const noexcept override {
        return output_ && !duplication_ && absent_ != RCS_CAPTURE_PAUSE_GONE ? RCS_CAPTURE_PAUSE_DESKTOP
                                                                             : rcs_capture_pause(absent_);
    }
    uint64_t interruptions() const noexcept override { return interruptions_; }
    Rect content() const noexcept override { return content_; }
    DisplayMode display() const noexcept override { return shared_; }
    void setCursorInVideo(bool inVideo) override { cursorInVideo_ = inVideo; }
    CursorState cursor() const override { return cursorState_; }
    std::shared_ptr<void> device() const noexcept override { return win::share(d3d_.device.value); }

    // A thumbnail another thread asks for, of this capture's display: taken
    // by next() on this capture's thread, answered through the job.
    void ask(const std::shared_ptr<ThumbnailJob>& job) {
        std::lock_guard lock(jobsMutex_);
        jobs_.push_back(job);
    }

    // On this capture's thread: a thumbnail of `request` (its own part of the
    // display) from the desktop image held now, or the next.
    Thumbnail snapshot(const ThumbnailRequest& request, Clock::time_point deadline) {
        if (Clock::now() >= deadline) {
            fail(RCS_ERROR_TIMEOUT, "The capture of the display did not get to the thumbnail within three seconds");
        }
        if (!duplication_) {
            fail(RCS_ERROR_INVALID_STATE, "Capture of the display is paused (a secure desktop, a mode change)");
        }
        if (!held_) {
            holdFrame(deadline);
        }
        // What of the display the picture shows: all of it, the region as far
        // as the display reaches, or where the application's windows are in front.
        const auto& target = request.target;
        RECT from{0, 0, LONG(display_.width), LONG(display_.height)};
        std::vector<RECT> visible;
        if (target.source == RCS_CAPTURE_REGION) {
            const auto& r = target.region;
            const RECT region{LONG(r.x), LONG(r.y), LONG(r.x + r.width), LONG(r.y + r.height)};
            const RECT whole = from;
            if (!IntersectRect(&from, &region, &whole)) {
                fail(RCS_ERROR_INVALID_STATE, "The region is off the display");
            }
        } else if (target.source == RCS_CAPTURE_APPLICATION) {
            const win::Handle process(OpenProcess(SYNCHRONIZE, FALSE, target.processId));
            if (!process || WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
                fail(RCS_ERROR_INVALID_STATE, "The application quit");
            }
            visible = win::visibleParts(target.processId, desktopRect_);
            if (visible.empty() && !win::showsOn(target.processId, desktopRect_)) {
                fail(RCS_ERROR_INVALID_STATE, "The application shows no window on the display");
            }
        }
        auto texture = heldResource_.as<ID3D11Texture2D>("Desktop texture", captureError);
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (!panelShaped(desc)) {
            fail(RCS_ERROR_INVALID_STATE, "The desktop changed (a mode change)");
        }
        return win::drawThumbnail(
            shader_, d3d_, texture.value, turns_, from, desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? white_ : 0,
            target.source == RCS_CAPTURE_APPLICATION ? &visible : nullptr, request.maxWidth, request.maxHeight);
    }

private:
    // Whether a desktop image has the panel's shape for the mode duplicated:
    // the desktop's, or its sides swapped where the display is turned sideways.
    bool panelShaped(const D3D11_TEXTURE2D_DESC& desc) const noexcept {
        const bool sideways = turns_ % 2;
        return desc.Width == (sideways ? display_.height : display_.width) &&
               desc.Height == (sideways ? display_.width : display_.height);
    }

    // The display, for this capture's duplication: a thumbnail duplicating it
    // for itself now ends first. Another capture of this process that has it
    // keeps it (DuplicateOutput then refuses this one, as it always did).
    void attach(CancelToken cancelled) {
        auto& d = duplications();
        std::unique_lock lock(d.mutex);
        while (d.pictures.count(displayId_)) {
            d.changed.wait_for(lock, 50ms);
            throwIfCancelled(cancelled);
        }
        registered_ = d.running.emplace(displayId_, this).second;
    }

    // Lets the display go; the thumbnails asked of this capture and not taken fail.
    void leave() noexcept {
        if (registered_) {
            auto& d = duplications();
            std::lock_guard lock(d.mutex);
            d.running.erase(displayId_);
            registered_ = false;
        }
        std::vector<std::shared_ptr<ThumbnailJob>> jobs;
        {
            std::lock_guard lock(jobsMutex_);
            jobs.swap(jobs_);
        }
        for (const auto& job : jobs) {
            job->answer(std::nullopt, std::make_exception_ptr(
                                          Failure(RCS_ERROR_INVALID_STATE, "The capture of the display stopped")));
        }
    }

    // The thumbnails other threads asked for, taken now.
    void serveThumbnails() {
        std::vector<std::shared_ptr<ThumbnailJob>> jobs;
        {
            std::lock_guard lock(jobsMutex_);
            jobs.swap(jobs_);
        }
        for (const auto& job : jobs) {
            std::optional<Thumbnail> picture;
            std::exception_ptr failure;
            try {
                picture = snapshot(job->request, job->deadline);
            } catch (...) {
                failure = std::current_exception();
            }
            job->answer(std::move(picture), failure);
        }
    }

    // Takes a desktop image and holds it, for a thumbnail and then next(): the
    // next one that comes within 100 ms, or (a still desktop brings none) the
    // whole desktop a new duplication shows at once. Where the duplication is
    // lost, the capture pauses as next() pauses for it.
    void holdFrame(Clock::time_point deadline) {
        // Only a frame that carries a desktop image will do: one that moves
        // the pointer alone (LastPresentTime 0) holds no picture, and the
        // first such frame of a new duplication is a black texture. A still
        // desktop brings no frame at all, so after 100 ms a new duplication
        // asks for the whole desktop again.
        bool duplicatedAgain = false;
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            if (left.count() <= 0) {
                fail(RCS_ERROR_INVALID_STATE,
                     "The display delivered no picture within three seconds: Windows composes nothing for it now "
                     "(a display that is off, or showing another input)");
            }
            HRESULT result = duplication_->AcquireNextFrame(
                UINT(duplicatedAgain ? left.count() : std::min<int64_t>(left.count(), 100)), &heldInfo_,
                heldResource_.out());
            if (result == DXGI_ERROR_WAIT_TIMEOUT) {
                if (duplicatedAgain) {
                    continue;
                }
                duplicatedAgain = true;
                duplication_.reset();
                if (const HRESULT duplicated = duplicate(); FAILED(duplicated)) {
                    ++interruptions_;
                    failDuplication(duplicated);
                }
                continue;
            }
            if (result == DXGI_ERROR_ACCESS_LOST ||
                (config_.secureDesktop && (result == DXGI_ERROR_INVALID_CALL || transient(result)))) {
                duplication_.reset();
                ++interruptions_;
                fail(RCS_ERROR_INVALID_STATE,
                     "The desktop changed (a secure desktop, a mode change; HRESULT " + hex(result) + ")");
            }
            win::check(result, "Desktop acquisition", captureError);
            if (heldInfo_.LastPresentTime.QuadPart) {
                held_ = true;
                return;
            }
            win::trace("thumbnail: a pointer-only frame, waiting for a picture");
            heldResource_.reset();
            duplication_->ReleaseFrame();
        }
    }

    void open(IDXGIAdapter1* adapter, IDXGIOutput* output) {
        DXGI_ADAPTER_DESC1 desc{};
        win::check(adapter->GetDesc1(&desc), "DXGI adapter description", captureError);
        adapterLuid_ = desc.AdapterLuid;
        d3d_.create(adapter);
        win::check(output->QueryInterface(__uuidof(IDXGIOutput5), reinterpret_cast<void**>(output_.out())),
                   "Desktop duplication interface", captureError);
        // Starting needs a capturable desktop; only a running capture pauses.
        const HRESULT result = duplicate();
        if (FAILED(result) && picture_) {
            failDuplication(result);
        }
        checkDuplication(result, "Desktop duplication (requires an interactive, unlocked desktop)");
    }

    // The DXGI output that shows `monitor` now and its adapter, from a new
    // factory (kept: once it is no longer current, the outputs changed);
    // false when none does.
    bool findOutput(HMONITOR monitor, Com<IDXGIAdapter1>& adapter, Com<IDXGIOutput>& output) {
        win::check(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory_.out())),
                   "DXGI factory", captureError);
        for (UINT a = 0; factory_->EnumAdapters1(a, adapter.out()) != DXGI_ERROR_NOT_FOUND; ++a) {
            for (UINT o = 0; adapter->EnumOutputs(o, output.out()) != DXGI_ERROR_NOT_FOUND; ++o) {
                DXGI_OUTPUT_DESC desc{};
                output->GetDesc(&desc);
                if (desc.Monitor == monitor) {
                    return true;
                }
            }
        }
        return false;
    }

    // Whether the display is connected. Where it came back, or the system's
    // outputs changed (the DXGI factory is no longer current), its output is
    // looked up again: on the GPU capture opened, since a capture does not
    // change device (the encoder runs on it); on another GPU capture fails.
    bool reconnect() {
        const auto monitor = win::findMonitor(displayId_);
        if (!monitor) {
            return false;
        }
        if (monitor->handle == monitor_ && factory_->IsCurrent()) {
            return true;
        }
        Com<IDXGIAdapter1> adapter;
        Com<IDXGIOutput> output;
        if (!findOutput(monitor->handle, adapter, output)) {
            return false;
        }
        DXGI_ADAPTER_DESC1 desc{};
        win::check(adapter->GetDesc1(&desc), "DXGI adapter description", captureError);
        if (desc.AdapterLuid.LowPart != adapterLuid_.LowPart || desc.AdapterLuid.HighPart != adapterLuid_.HighPart) {
            fail(captureError, "The shared display is now driven by another GPU, and a capture keeps the GPU it "
                               "started on: start capture anew");
        }
        win::check(output->QueryInterface(__uuidof(IDXGIOutput5), reinterpret_cast<void**>(output_.out())),
                   "Desktop duplication interface", captureError);
        monitor_ = monitor->handle;
        return true;
    }

    // DuplicateOutput1 (Windows 10 1703) delivers FP16 HDR desktops as they
    // are and updates live while a window is dragged; the older DuplicateOutput
    // shows the pre-drag image on an HDR desktop until the drag ends.
    HRESULT duplicate() {
        win::DpiScope dpi; // The display's size in its pixels, also for a DPI-unaware host.
        const DXGI_FORMAT formats[]{DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
        const HRESULT result = output_->DuplicateOutput1(d3d_.device.value, 0, 2, formats, duplication_.out());
        if (FAILED(result)) {
            return result;
        }
        // The desktop's texture, size, pixel format and brightness may differ
        // after a mode change.
        sourceView_.reset();
        viewed_ = nullptr;
        DXGI_OUTDUPL_DESC duplicated{};
        duplication_->GetDesc(&duplicated);
        hdr_ = duplicated.ModeDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (hdr_) {
            readSdrWhite();
        }
        // The display's size in its pixels and its place on the virtual
        // desktop: what DXGI says of the output itself is scaled for a process
        // that is not DPI aware, whatever the thread's awareness.
        const auto monitor = win::findMonitor(displayId_);
        if (!monitor) {
            duplication_.reset();
            return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE; // Being reconfigured: later.
        }
        desktopRect_ = monitor->rect;
        display_.width = duplicated.ModeDesc.Width; // The desktop's orientation...
        display_.height = duplicated.ModeDesc.Height;
        // ...and the texture's, turned this far (ROTATE90 is 2, IDENTITY 1, UNSPECIFIED 0).
        turns_ = duplicated.Rotation >= DXGI_MODE_ROTATION_ROTATE90 ? UINT(duplicated.Rotation) - 1 : 0;
        // What of the desktop is shared: all of it, or the region as far as
        // the display reaches now.
        from_ = {0, 0, LONG(display_.width), LONG(display_.height)};
        if (config_.target.source == RCS_CAPTURE_REGION) {
            const auto& r = config_.target.region;
            const RECT region{LONG(r.x), LONG(r.y), LONG(r.x + r.width), LONG(r.y + r.height)};
            const RECT whole = from_;
            if (!IntersectRect(&from_, &region, &whole)) {
                absent_ = RCS_CAPTURE_PAUSE_HIDDEN;
                from_ = region;
            } else if (absent_ == RCS_CAPTURE_PAUSE_HIDDEN) {
                absent_ = RCS_CAPTURE_PAUSE_NONE;
            }
        }
        shared_ = {uint32_t(from_.right - from_.left), uint32_t(from_.bottom - from_.top), display_.refreshHz};
        const auto geometry = viewGeometry(int(config_.width), int(config_.height), shared_.width, shared_.height);
        content_ = {uint32_t(geometry.x), uint32_t(geometry.y), uint32_t(geometry.width), uint32_t(geometry.height)};
        visibleKnown_ = false;
        return S_OK;
    }

    // APPLICATION: where its windows show now. False when there is nothing to
    // draw: none of its windows on the display (HIDDEN), or its process ended
    // (GONE).
    bool followApplication() {
        if (WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
            log::info("capture", "Capture paused: the shared application quit");
            absent_ = RCS_CAPTURE_PAUSE_GONE;
            return false;
        }
        visible_ = win::visibleParts(config_.target.processId, desktopRect_);
        visibleKnown_ = true;
        const bool shows = !visible_.empty() || win::showsOn(config_.target.processId, desktopRect_);
        absent_ = shows ? RCS_CAPTURE_PAUSE_NONE : RCS_CAPTURE_PAUSE_HIDDEN;
        return shows;
    }

    // SDR white of an HDR desktop, read again every two seconds: the user may
    // move the "SDR content brightness" slider without a mode change.
    void readSdrWhite() {
        nextWhiteRead_ = Clock::now() + 2s;
        const auto white = sdrWhite(device_);
        if (!white && !whiteUnknown_) {
            log::warning("capture", "Windows does not say how bright SDR content is on this HDR display; SDR white is "
                                    "taken as 80 nits, and brighter SDR content clips");
        }
        whiteUnknown_ = !white;
        white_ = white.value_or(1);
    }

    // While paused, tries to duplicate the output again every 250 ms; false
    // (after waiting up to `timeout`) while the desktop is still uncapturable.
    bool resume(std::chrono::milliseconds timeout) {
        const auto now = Clock::now();
        if (now < nextAttempt_) {
            sleepPrecise(std::min<std::chrono::nanoseconds>(nextAttempt_ - now, timeout));
            return false;
        }
        nextAttempt_ = now + 250ms;
        if (config_.secureDesktop) {
            // The desktop that receives input changed (a lock screen, a UAC
            // prompt, the user's again): this thread goes there before it
            // duplicates the output. A desktop that is being switched may be
            // closed for a moment; one that stays closed fails capture.
            if (!desktop_.attach()) {
                if (++attachFailures_ >= 40) {
                    fail(captureError, "Capture cannot follow the desktop that receives input (Windows error " +
                                           std::to_string(desktop_.error()) + ")");
                }
                return false;
            }
            attachFailures_ = 0;
            if (desktop_.name() != followed_) {
                followed_ = desktop_.name();
                log::info("capture", "Capture follows the input desktop \"" + win::narrow(followed_) + "\"");
            }
        }
        // A disconnected display pauses capture, and fails it after 10 s as on macOS.
        if (!reconnect()) {
            if (!disconnectedSince_) {
                disconnectedSince_ = now;
                log::info("capture", "Capture paused: the shared display is disconnected");
            }
            if (now - *disconnectedSince_ >= disconnectedLimit) {
                fail(captureError, "The shared display was disconnected");
            }
            return false;
        }
        disconnectedSince_.reset();
        const HRESULT result = duplicate();
        if (FAILED(result) && transient(result)) {
            return false;
        }
        checkDuplication(result, "Desktop duplication");
        win::trace("capture resumed");
        return true;
    }

    // Hides this process's top-level windows that captures still show; true
    // when it hid one. Windows already kept out (by an earlier call, or by the
    // host with WDA_MONITOR) stay as they are. A visible window that cannot be
    // hidden fails capture: sharing it would ignore the host's setting.
    bool hideOwnWindows() {
        struct Scan {
            std::vector<HWND>& hidden;
            bool changed = false, refused = false;
            DWORD error = ERROR_SUCCESS;
        } scan{hidden_};
        EnumWindows(
            [](HWND window, LPARAM data) -> BOOL {
                auto& s = *reinterpret_cast<Scan*>(data);
                DWORD process = 0, affinity = WDA_NONE;
                GetWindowThreadProcessId(window, &process);
                if (process != GetCurrentProcessId() ||
                    (GetWindowDisplayAffinity(window, &affinity) && affinity != WDA_NONE)) {
                    return TRUE;
                }
                if (SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE)) {
                    s.hidden.push_back(window);
                    s.changed = true;
                } else if (IsWindowVisible(window)) {
                    s.refused = true;
                    s.error = GetLastError();
                    return FALSE;
                }
                return TRUE; // Invisible or gone: tried again at the next update.
            },
            reinterpret_cast<LPARAM>(&scan));
        if (scan.refused) {
            fail(captureError, "A window of this application cannot be hidden from the shared picture (Windows error " +
                                   std::to_string(scan.error) +
                                   "); close it, or set exclude_current_process to 0 to share it");
        }
        return scan.changed;
    }

    void updateCursor(const DXGI_OUTDUPL_FRAME_INFO& info) {
        if (info.LastMouseUpdateTime.QuadPart) {
            cursorPoint_ = info.PointerPosition.Position;
            cursorVisible_ = info.PointerPosition.Visible != FALSE;
        }
        if (info.PointerShapeBufferSize) {
            updateCursorShape(info);
        }
        if (info.LastMouseUpdateTime.QuadPart || info.PointerShapeBufferSize) {
            reportCursor();
        }
    }

    // The pointer for cursor(): its hotspot across what is shared, and its image.
    void reportCursor() {
        const POINT hotspot{cursorPoint_.x + LONG(cursorShape_.HotSpot.x),
                            cursorPoint_.y + LONG(cursorShape_.HotSpot.y)};
        cursorState_.visible = cursorVisible_ && cursorView_ && PtInRect(&from_, hotspot);
        const auto across = [](LONG offset, LONG size) {
            return size > 1 ? uint32_t(std::clamp<int64_t>(int64_t(offset) * 65535 / (size - 1), 0, 65535)) : 0u;
        };
        cursorState_.x = across(hotspot.x - from_.left, from_.right - from_.left);
        cursorState_.y = across(hotspot.y - from_.top, from_.bottom - from_.top);
    }

    void updateCursorShape(const DXGI_OUTDUPL_FRAME_INFO& info) {
        std::vector<uint8_t> shape(info.PointerShapeBufferSize);
        UINT size = 0;
        win::check(duplication_->GetFramePointerShape(UINT(shape.size()), shape.data(), &size, &cursorShape_),
                   "Cursor shape", captureError);
        const bool monochrome = cursorShape_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
        cursorWidth_ = cursorShape_.Width;
        cursorHeight_ = monochrome ? cursorShape_.Height / 2 : cursorShape_.Height;
        if (!cursorWidth_ || !cursorHeight_) {
            cursorView_.reset();
            cursorVisible_ = false;
            return;
        }
        auto image = std::make_shared<CursorImage>();
        image->width = cursorWidth_;
        image->height = cursorHeight_;
        image->hotspotX = std::min<UINT>(cursorShape_.HotSpot.x, cursorWidth_ - 1);
        image->hotspotY = std::min<UINT>(cursorShape_.HotSpot.y, cursorHeight_ - 1);
        image->pixels = straightPixels(shape, cursorShape_, cursorWidth_, cursorHeight_);
        fitCursor(*image); // Sent apart at most maxCursorSize per side; drawn into the video as it is.
        CURSORINFO current{};
        current.cbSize = sizeof(current);
        image->type = GetCursorInfo(&current) ? win::standardType(current.hCursor) : RCS_CURSOR_CUSTOM;
        cursorState_.image = std::move(image);
        ++cursorState_.imageSerial;
        // Monochrome cursors hold an AND mask above an XOR mask; expand to bytes.
        std::vector<uint8_t> pixels(size_t(cursorWidth_) * cursorHeight_ * 4);
        for (UINT y = 0; y < cursorHeight_; ++y) {
            for (UINT x = 0; x < cursorWidth_; ++x) {
                uint8_t* p = pixels.data() + (size_t(y) * cursorWidth_ + x) * 4;
                if (monochrome) {
                    const uint8_t bit = uint8_t(0x80 >> (x % 8));
                    p[1] = (shape[size_t(y + cursorHeight_) * cursorShape_.Pitch + x / 8] & bit) ? 255 : 0;
                    p[2] = (shape[size_t(y) * cursorShape_.Pitch + x / 8] & bit) ? 255 : 0;
                    p[3] = 255;
                } else {
                    std::memcpy(p, shape.data() + size_t(y) * cursorShape_.Pitch + x * 4, 4);
                }
            }
        }
        auto desc =
            win::textureDesc(cursorWidth_, cursorHeight_, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        const D3D11_SUBRESOURCE_DATA data{pixels.data(), cursorWidth_ * 4, 0};
        Com<ID3D11Texture2D> texture;
        win::check(d3d_.device->CreateTexture2D(&desc, &data, texture.out()), "Cursor texture", captureError);
        win::check(d3d_.device->CreateShaderResourceView(texture.value, nullptr, cursorView_.out()), "Cursor view",
                   captureError);
    }

    CaptureConfig config_;
    win::InputDesktop desktop_; // With secure_desktop: the desktop this thread is on.
    std::wstring followed_;     // The name logged last.
    int attachFailures_ = 0;
    win::Device d3d_;
    std::wstring device_;    // \\.\DISPLAYN of the display captured...
    uint32_t displayId_ = 0; // ...and its id.
    HMONITOR monitor_ = nullptr; // The display's handle when its output was found...
    Com<IDXGIFactory1> factory_; // ...by this factory...
    Com<IDXGIOutput5> output_;   // ...on the GPU capture opened.
    LUID adapterLuid_{};
    std::optional<Clock::time_point> disconnectedSince_;
    Com<IDXGIOutputDuplication> duplication_; // Null while paused.
    Clock::time_point nextAttempt_{};
    uint64_t interruptions_ = 0;
    Com<ID3D11ShaderResourceView> sourceView_; // Of the duplicated desktop texture...
    ID3D11Texture2D* viewed_ = nullptr;        // ...this one.
    win::PicturePool pictures_;
    bool hdr_ = false;           // The desktop is scRGB (FP16)...
    float white_ = 1;            // ...with SDR white at this value...
    bool whiteUnknown_ = false;  // ...or Windows did not say.
    Clock::time_point nextWhiteRead_{};
    win::SurfaceShader shader_;
    Rect content_;
    uint64_t frames_ = 0, overwritten_ = 0;

    Com<ID3D11ShaderResourceView> cursorView_;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO cursorShape_{};
    UINT cursorWidth_ = 0, cursorHeight_ = 0;
    POINT cursorPoint_{};
    bool cursorVisible_ = false;
    bool cursorInVideo_ = true;
    CursorState cursorState_;
    DisplayMode display_; // The display's own, as the desktop shows it...
    UINT turns_ = 0;      // ...how far it is turned (its texture is the panel's)...
    RECT desktopRect_{};  // ...its place on the virtual desktop...
    RECT from_{};         // ...and the part of it shared, in its pixels...
    DisplayMode shared_;  // ...whose size display() says.

    // What is shared that does not show now (a region off the display, an
    // application without windows there or gone); read on any thread.
    std::atomic<rcs_capture_pause> absent_{RCS_CAPTURE_PAUSE_NONE};
    HANDLE process_ = nullptr;  // APPLICATION: its process, to see it end...
    std::vector<RECT> visible_; // ...and where its windows show (the display's pixels).
    bool visibleKnown_ = false;

    std::vector<HWND> hidden_;  // Restored to WDA_NONE when capture stops.
    int64_t withheldUntil_ = 0; // Pictures presented earlier may show a window just hidden.

    // Thumbnails (see the top of the file).
    const bool picture_;      // This capture is for one thumbnail...
    bool registered_ = false; // ...or a stream's, which the display's thumbnails ask.
    std::mutex jobsMutex_;
    std::vector<std::shared_ptr<ThumbnailJob>> jobs_;
    bool held_ = false; // A desktop image a thumbnail took, held for next().
    DXGI_OUTDUPL_FRAME_INFO heldInfo_{};
    Com<IDXGIResource> heldResource_;
};

// DISPLAY, REGION, APPLICATION: from the capture of this process that runs on
// the display, or from a duplication of the display for this picture alone.
Thumbnail desktopThumbnail(const ThumbnailRequest& request) {
    const auto deadline = Clock::now() + 3s;
    const auto monitor = win::findMonitor(request.target.displayId);
    if (!monitor) {
        fail(RCS_ERROR_INVALID_STATE, "The display is not attached now");
    }
    auto& d = duplications();
    // The capture that runs on the display takes it, if there is one (holding d.mutex).
    const auto viaCapture = [&](std::unique_lock<std::mutex>& lock) -> std::optional<Thumbnail> {
        const auto running = d.running.find(monitor->id);
        if (running == d.running.end()) {
            return std::nullopt;
        }
        auto job = std::make_shared<ThumbnailJob>();
        job->request = request;
        job->deadline = deadline;
        running->second->ask(job);
        lock.unlock();
        std::unique_lock answer(job->mutex);
        if (!job->answered.wait_until(answer, deadline + 1s, [&] { return job->done; })) {
            fail(RCS_ERROR_TIMEOUT, "The capture of the display did not take the thumbnail within three seconds "
                                    "(its stream is held up)");
        }
        if (job->failure) {
            std::rethrow_exception(job->failure);
        }
        return std::move(job->picture);
    };
    {
        std::unique_lock lock(d.mutex);
        if (auto picture = viaCapture(lock)) {
            return std::move(*picture);
        }
    }
    std::lock_guard one(d.oneAtATime);
    {
        std::unique_lock lock(d.mutex);
        if (auto picture = viaCapture(lock)) {
            return std::move(*picture);
        }
        d.pictures.insert(monitor->id);
    }
    const struct Release { // A capture waiting for the display goes on.
        Duplications& d;
        uint32_t id;
        ~Release() {
            {
                std::lock_guard lock(d.mutex);
                d.pictures.erase(id);
            }
            d.changed.notify_all();
        }
    } release{d, monitor->id};
    return win::onThread([&] {
        static const std::atomic<bool> never{false};
        CaptureConfig config;
        config.width = config.height = 16; // A stream's; none is made.
        config.target.displayId = monitor->id;
        config.cursorInVideo = false;
        config.secureDesktop = request.secureDesktop;
        DesktopDuplicationCapture capture(true);
        capture.start(config, never);
        return capture.snapshot(request, deadline);
    });
}

} // namespace

std::unique_ptr<ScreenCapture> createScreenCapture(const CaptureTarget& target) {
    if (target.source == RCS_CAPTURE_WINDOW) {
        return win::createWindowCapture();
    }
    return std::make_unique<DesktopDuplicationCapture>();
}

Thumbnail captureThumbnail(const ThumbnailRequest& request) {
    const bool window = request.target.source == RCS_CAPTURE_WINDOW;
    if (request.excludeCurrentProcess && !window) {
        fail(RCS_ERROR_UNSUPPORTED,
             "RCS_EXT_CAPTURE: exclude_current_process: one picture of desktop duplication cannot leave this "
             "application's windows out (a running capture hides them window by window and waits for the desktop "
             "to update without them); take the thumbnail with exclude_current_process 0");
    }
    if (!request.secureDesktop && desktopState().kind == RCS_DESKTOP_SECURE) {
        fail(RCS_ERROR_INVALID_STATE, "A secure desktop (the sign-in or lock screen, a UAC prompt) is in front");
    }
    return window ? win::windowThumbnail(request) : desktopThumbnail(request);
}

} // namespace rcsdk::platform
