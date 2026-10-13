// Capture of one window with Windows.Graphics.Capture (Windows 10 1903 or
// later): the window as the compositor draws it, also where other windows
// cover it or past a display's edge, at the size of its visible frame, which
// the picture follows (the frame pool is made anew for a new size). The
// border Windows draws around a captured window is turned off where the
// system lets it (Windows 11). Frames come only when the window changes;
// next() polls the pool for them.
//
// A minimized, hidden or cloaked window pauses capture (HIDDEN), and so does
// a secure desktop or lock screen in front of it (DESKTOP); the first frames
// after it shows again may still be of its minimized form, and are passed
// over. A window that no longer exists (or whose handle names another
// process's window now) pauses capture for good (GONE). With the cursor sent
// apart, Windows leaves it out and GDI reports it.
#include "platform/windows/window_capture.hpp"

#include "platform/windows/cursor.hpp"
#include "platform/windows/d3d.hpp"
#include "platform/windows/picture_pool.hpp"
#include "platform/windows/surface_shader.hpp"
#include "platform/windows/thumbnail.hpp"
#include "platform/windows/window.hpp"

#include "util/log.hpp"

#include <rcsdk/view_geometry.hpp>

#ifdef __MINGW32__
// mingw-w64's headers define IReference<boolean> and IReference<BYTE>, which are one type.
#define ____FIReference_1_boolean_INTERFACE_DEFINED__
#endif
#include <roapi.h>
#include <winstring.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <dxgi1_2.h>

#include <atomic>
#include <map>
#include <thread>

namespace rcsdk::platform::win {
namespace {

namespace capture = ABI::Windows::Graphics::Capture;
namespace direct3d = ABI::Windows::Graphics::DirectX::Direct3D11;
using ABI::Windows::Graphics::SizeInt32;
using Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

constexpr rcs_result captureError = RCS_ERROR_CAPTURE;
constexpr auto pixelFormat = ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized;

// Windows.Graphics.Capture, kept for the process's life: the multithreaded
// apartment it lives in (a thread that joins no apartment of its own uses it,
// and it is not torn down and made anew between captures), and its DLL. COM
// unloads the DLL once nothing of it is in use, while work it queued may still
// run: the next capture then executes unloaded code (seen on Windows 11
// 24H2, one capture after another in one process).
bool keepApartment() {
    static const bool kept = [] {
        CO_MTA_USAGE_COOKIE cookie{};
        if (FAILED(CoIncrementMTAUsage(&cookie))) {
            return false;
        }
        HMODULE module = nullptr;
        return LoadLibraryExW(L"GraphicsCapture.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) &&
               GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"GraphicsCapture.dll", &module);
    }();
    return kept;
}

template <class T>
Com<T> factory(const wchar_t* name) {
    HSTRING_HEADER header;
    HSTRING type = nullptr;
    Com<T> result;
    if (SUCCEEDED(WindowsCreateStringReference(name, UINT32(wcslen(name)), &header, &type))) {
        RoGetActivationFactory(type, __uuidof(T), reinterpret_cast<void**>(result.out()));
    }
    return result;
}

template <class T, class U>
Com<T> query(const Com<U>& object) {
    Com<T> result;
    if (object) {
        object->QueryInterface(__uuidof(T), reinterpret_cast<void**>(result.out()));
    }
    return result;
}

// The adapter driving `monitor`; null when none says so (the default then).
Com<IDXGIAdapter1> adapterOf(HMONITOR monitor) {
    Com<IDXGIFactory1> dxgi;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(dxgi.out())))) {
        return {};
    }
    for (UINT a = 0;; ++a) {
        Com<IDXGIAdapter1> adapter;
        if (dxgi->EnumAdapters1(a, adapter.out()) == DXGI_ERROR_NOT_FOUND) {
            return {};
        }
        for (UINT o = 0;; ++o) {
            Com<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, output.out()) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
                return adapter;
            }
        }
    }
}

uint32_t refreshOf(HMONITOR monitor) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (GetMonitorInfoW(monitor, &info) && EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
        return mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0;
    }
    return 0;
}

const char* pauseName(rcs_capture_pause pause) {
    switch (pause) {
    case RCS_CAPTURE_PAUSE_NONE:
        return "it shows again";
    case RCS_CAPTURE_PAUSE_HIDDEN:
        return "minimized or hidden";
    case RCS_CAPTURE_PAUSE_GONE:
        return "it is gone";
    default:
        return "a secure desktop is in front";
    }
}

class GraphicsCaptureWindow final : public ScreenCapture {
public:
    ~GraphicsCaptureWindow() override { stop(); }

    void start(const CaptureConfig& config, CancelToken) override {
        config_ = config;
        cursorInVideo_ = config.cursorInVideo;
        pace(config.framesPerSecond);
        content_ = {0, 0, config.width, config.height}; // Until a window shows: the whole frame.
        window_ = windowOf(config.target.windowId);
        process_ = processOf(window_);
        // The GPU of the display that shows the window now (the main one when
        // it is gone: the encoder opens on it all the same); Windows hands
        // frames across GPUs when the window moves.
        const HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTOPRIMARY);
        refresh_ = refreshOf(monitor);
        d3d_.create(adapterOf(monitor).value);
        auto dxgi = d3d_.device.as<IDXGIDevice>("DXGI device", captureError);
        Com<IInspectable> inspectable;
        check(CreateDirect3D11DeviceFromDXGIDevice(dxgi.value, inspectable.out()), "Window capture device",
              captureError);
        check(inspectable->QueryInterface(__uuidof(direct3d::IDirect3DDevice), reinterpret_cast<void**>(device_.out())),
              "Window capture device", captureError);
        const auto state = process_ ? windowState(window_, process_) : std::nullopt;
        if (!state) {
            hidden_ = RCS_CAPTURE_PAUSE_GONE; // Gone before capture started: it starts paused, for good.
            return;
        }
        frame_ = state->frame;
        open();
        look();
    }

    bool next(VideoFrame& frame, std::chrono::milliseconds timeout, CancelToken cancelled) override {
        const auto deadline = Clock::now() + timeout;
        if (hidden_ != RCS_CAPTURE_PAUSE_GONE && Clock::now() >= nextLook_) {
            look();
        }
        if (!cursorInVideo_) {
            cursor_.poll(frame_);
        }
        if (hidden_ != RCS_CAPTURE_PAUSE_NONE) {
            std::this_thread::sleep_for(std::min<std::chrono::milliseconds>(timeout, 20ms));
            if (pool_) { // What came meanwhile is of the window as it was.
                for (Com<capture::IDirect3D11CaptureFrame> stale;
                     SUCCEEDED(pool_->TryGetNextFrame(stale.out())) && stale;) {
                }
            }
            return false;
        }
        // Paced to the configured rate (due()).
        const auto slot = due();
        for (auto left = slot - Clock::now(); left.count() > 0 && !cancelled; left = slot - Clock::now()) {
            sleepPrecise(std::min<std::chrono::nanoseconds>(left, 4ms));
        }
        throwIfCancelled(cancelled);
        // The newest frame that came, waiting up to the deadline for one.
        Com<capture::IDirect3D11CaptureFrame> latest;
        for (;;) {
            for (Com<capture::IDirect3D11CaptureFrame> arrived;
                 SUCCEEDED(pool_->TryGetNextFrame(arrived.out())) && arrived;) {
                overwritten_ += latest ? 1 : 0;
                latest = std::move(arrived);
            }
            if (latest || Clock::now() >= deadline || cancelled) {
                break;
            }
            sleepPrecise(1ms);
        }
        if (!latest) {
            return false;
        }
        SizeInt32 size{};
        check(latest->get_ContentSize(&size), "Window frame size", captureError);
        if (size.Width <= 0 || size.Height <= 0 || stale(size)) {
            return false;
        }
        Com<direct3d::IDirect3DSurface> surface;
        if (FAILED(latest->get_Surface(surface.out())) || !surface) {
            return false; // The window went as the frame came.
        }
        auto access = surface.as<IDirect3DDxgiInterfaceAccess>("Window frame texture", captureError);
        Com<ID3D11Texture2D> texture;
        check(access->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture.out())),
              "Window frame texture", captureError);
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (uint32_t(size.Width) != shared_.width || uint32_t(size.Height) != shared_.height) {
            place(size);
        }

        auto output = pictures_.next(d3d_.device.value, config_.width, config_.height);
        SurfaceDraw draw;
        draw.source = viewOf(texture.value);
        draw.sourceWidth = desc.Width;
        draw.sourceHeight = desc.Height;
        // A window larger than the pool shows cut until the pool is made anew.
        draw.from = {0, 0, std::min<LONG>(size.Width, LONG(desc.Width)),
                     std::min<LONG>(size.Height, LONG(desc.Height))};
        draw.target = output.target.value;
        draw.to = {LONG(content_.x), LONG(content_.y), LONG(content_.x + content_.width),
                   LONG(content_.y + content_.height)};
        shader_.draw(d3d_.device.value, d3d_.context.value, draw);
        latest.reset(); // Back to the pool, after the draw that reads it is queued.
        if (size.Width != poolSize_.Width || size.Height != poolSize_.Height) {
            check(pool_->Recreate(device_.value, pixelFormat, 2, size), "Window frame pool", captureError);
            poolSize_ = size;
            views_.clear();
        }

        took();
        frame.width = config_.width;
        frame.height = config_.height;
        frame.sequence = ++frames_;
        frame.timestampNs = monotonicNs();
        frame.surface = std::move(output.surface);
        frame.device = share(d3d_.device.value);
        frame.waitGpu = gpuFence(d3d_.device.value, d3d_.context.value);
        return true;
    }

    void stop() noexcept override {
        close();
        pictures_.clear();
        device_.reset();
        d3d_ = {};
    }

    void setCursorInVideo(bool inVideo) override {
        cursorInVideo_ = inVideo;
        if (const auto session = query<capture::IGraphicsCaptureSession2>(session_)) {
            session->put_IsCursorCaptureEnabled(inVideo);
        }
    }
    CursorState cursor() const override { return cursor_.state(); }
    uint64_t captured() const noexcept override { return frames_; }
    uint64_t overwritten() const noexcept override { return overwritten_; }
    rcs_capture_pause paused() const noexcept override { return hidden_; }
    uint64_t interruptions() const noexcept override { return interruptions_; }
    Rect content() const noexcept override { return content_; }
    DisplayMode display() const noexcept override { return shared_; }
    std::shared_ptr<void> device() const noexcept override { return share(d3d_.device.value); }

private:
    void open() {
        const auto interop = factory<IGraphicsCaptureItemInterop>(L"Windows.Graphics.Capture.GraphicsCaptureItem");
        const auto pools = factory<capture::IDirect3D11CaptureFramePoolStatics2>(
            L"Windows.Graphics.Capture.Direct3D11CaptureFramePool");
        if (!interop || !pools) {
            fail(RCS_ERROR_UNSUPPORTED, "This system cannot capture single windows (Windows 10 1903 or later)");
        }
        const HRESULT made = interop->CreateForWindow(window_, __uuidof(capture::IGraphicsCaptureItem),
                                                      reinterpret_cast<void**>(item_.out()));
        if (FAILED(made) && !windowState(window_, process_)) {
            hidden_ = RCS_CAPTURE_PAUSE_GONE;
            return;
        }
        check(made, "Capturing the window", captureError);
        check(item_->get_Size(&poolSize_), "Window size", captureError);
        check(pools->CreateFreeThreaded(device_.value, pixelFormat, 2, poolSize_, pool_.out()), "Window frame pool",
              captureError);
        check(pool_->CreateCaptureSession(item_.value, session_.out()), "Window capture session", captureError);
        if (const auto cursor = query<capture::IGraphicsCaptureSession2>(session_)) {
            cursor->put_IsCursorCaptureEnabled(cursorInVideo_);
        }
        const auto border = query<capture::IGraphicsCaptureSession3>(session_);
        if (!border || FAILED(border->put_IsBorderRequired(false))) {
            log::info("capture", "Windows draws a border around the shared window (it can leave it out from "
                                 "Windows 11 on)");
        }
        check(session_->StartCapture(), "Starting window capture", captureError);
        place(poolSize_);
    }

    // Ends the capture session and its pool.
    void close() noexcept {
        for (IUnknown* object : {static_cast<IUnknown*>(session_.value), static_cast<IUnknown*>(pool_.value)}) {
            Com<ABI::Windows::Foundation::IClosable> closable;
            if (object && SUCCEEDED(object->QueryInterface(__uuidof(ABI::Windows::Foundation::IClosable),
                                                           reinterpret_cast<void**>(closable.out())))) {
                closable->Close();
            }
        }
        session_.reset();
        pool_.reset();
        item_.reset();
        views_.clear();
    }

    // The window's state, a few times a second: whether it shows, where.
    void look() {
        nextLook_ = Clock::now() + 100ms;
        const auto state = windowState(window_, process_);
        rcs_capture_pause now = RCS_CAPTURE_PAUSE_NONE;
        if (!state) {
            now = RCS_CAPTURE_PAUSE_GONE;
        } else if (!state->shown) {
            now = RCS_CAPTURE_PAUSE_HIDDEN;
        } else if (desktopState().kind == RCS_DESKTOP_SECURE) {
            now = RCS_CAPTURE_PAUSE_DESKTOP;
        }
        if (state) {
            frame_ = state->frame;
        }
        if (now == hidden_) {
            return;
        }
        log::info("capture",
                  std::string(now == RCS_CAPTURE_PAUSE_NONE ? "Window capture resumed: " : "Window capture paused: ") +
                      pauseName(now));
        if (hidden_ == RCS_CAPTURE_PAUSE_NONE) {
            ++interruptions_;
        }
        if (now == RCS_CAPTURE_PAUSE_NONE) {
            restoredUntil_ = Clock::now() + 1s;
        }
        hidden_ = now;
        if (now == RCS_CAPTURE_PAUSE_GONE) {
            close();
        }
    }

    // Shortly after the window shows again, a frame may still be of it as it
    // was (minimized: its caption alone); the window's frame says its size.
    bool stale(const SizeInt32& size) const {
        return Clock::now() < restoredUntil_ && (std::abs(size.Width - (frame_.right - frame_.left)) > 2 ||
                                                 std::abs(size.Height - (frame_.bottom - frame_.top)) > 2);
    }

    // The window of this size, fitted into the picture.
    void place(const SizeInt32& size) {
        shared_ = {uint32_t(size.Width), uint32_t(size.Height), refresh_};
        const auto geometry = viewGeometry(int(config_.width), int(config_.height), shared_.width, shared_.height);
        content_ = {uint32_t(geometry.x), uint32_t(geometry.y), uint32_t(geometry.width), uint32_t(geometry.height)};
    }

    // A view of each texture of the pool, made once.
    ID3D11ShaderResourceView* viewOf(ID3D11Texture2D* texture) {
        auto& view = views_[texture];
        if (!view) {
            check(d3d_.device->CreateShaderResourceView(texture, nullptr, view.out()), "Window frame view",
                  captureError);
        }
        return view.value;
    }

    CaptureConfig config_;
    HWND window_ = nullptr;
    DWORD process_ = 0;
    RECT frame_{}; // Where the window is, as last looked at.
    uint32_t refresh_ = 0;
    std::atomic<rcs_capture_pause> hidden_{RCS_CAPTURE_PAUSE_NONE};
    Clock::time_point nextLook_{}, restoredUntil_{};

    Device d3d_;
    Com<direct3d::IDirect3DDevice> device_;
    Com<capture::IGraphicsCaptureItem> item_;
    Com<capture::IDirect3D11CaptureFramePool> pool_;
    Com<capture::IGraphicsCaptureSession> session_;
    SizeInt32 poolSize_{};
    std::map<ID3D11Texture2D*, Com<ID3D11ShaderResourceView>> views_; // Alive while the pool is.
    SurfaceShader shader_;
    PicturePool pictures_;
    Rect content_;
    DisplayMode shared_;

    bool cursorInVideo_ = true;
    CursorReader cursor_;
    uint64_t frames_ = 0, overwritten_ = 0, interruptions_ = 0;
};

} // namespace

bool windowCaptureAvailable() noexcept {
    static const bool available = [] { // Asked once.
        if (!keepApartment()) {
            return false;
        }
        const auto sessions =
            factory<capture::IGraphicsCaptureSessionStatics>(L"Windows.Graphics.Capture.GraphicsCaptureSession");
        boolean supported = false;
        return sessions && SUCCEEDED(sessions->IsSupported(&supported)) && supported &&
               factory<IGraphicsCaptureItemInterop>(L"Windows.Graphics.Capture.GraphicsCaptureItem");
    }();
    return available;
}

std::unique_ptr<ScreenCapture> createWindowCapture() {
    if (!windowCaptureAvailable()) {
        fail(RCS_ERROR_UNSUPPORTED, "This system cannot capture single windows (Windows 10 1903 or later)");
    }
    return std::make_unique<GraphicsCaptureWindow>();
}

// One picture: a capture session of its own that ends with its first frame
// (Windows draws a frame's first picture at once, then only on changes).
// What a running capture pauses for is looked at first: a minimized window
// would deliver only its caption.
Thumbnail windowThumbnail(const ThumbnailRequest& request) {
    if (!windowCaptureAvailable()) {
        fail(RCS_ERROR_UNSUPPORTED, "This system cannot capture single windows (Windows 10 1903 or later)");
    }
    return onThread([&] {
        const auto deadline = Clock::now() + 3s;
        const HWND window = windowOf(request.target.windowId);
        const DWORD process = processOf(window);
        const auto state = process ? windowState(window, process) : std::nullopt;
        if (!state) {
            fail(RCS_ERROR_INVALID_STATE, "The window is gone");
        }
        if (!state->shown) {
            fail(RCS_ERROR_INVALID_STATE, "The window does not show: it is minimized, hidden or cloaked");
        }
        const auto interop = factory<IGraphicsCaptureItemInterop>(L"Windows.Graphics.Capture.GraphicsCaptureItem");
        const auto pools = factory<capture::IDirect3D11CaptureFramePoolStatics2>(
            L"Windows.Graphics.Capture.Direct3D11CaptureFramePool");
        if (!interop || !pools) {
            fail(RCS_ERROR_UNSUPPORTED, "This system cannot capture single windows (Windows 10 1903 or later)");
        }
        Device d3d;
        d3d.create(adapterOf(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY)).value);
        auto dxgi = d3d.device.as<IDXGIDevice>("DXGI device", captureError);
        Com<IInspectable> inspectable;
        check(CreateDirect3D11DeviceFromDXGIDevice(dxgi.value, inspectable.out()), "Window capture device",
              captureError);
        Com<direct3d::IDirect3DDevice> device;
        check(inspectable->QueryInterface(__uuidof(direct3d::IDirect3DDevice), reinterpret_cast<void**>(device.out())),
              "Window capture device", captureError);
        Com<capture::IGraphicsCaptureItem> item;
        const HRESULT made = interop->CreateForWindow(window, __uuidof(capture::IGraphicsCaptureItem),
                                                      reinterpret_cast<void**>(item.out()));
        if (FAILED(made) && !windowState(window, process)) {
            fail(RCS_ERROR_INVALID_STATE, "The window is gone");
        }
        check(made, "Capturing the window", captureError);
        SizeInt32 size{};
        check(item->get_Size(&size), "Window size", captureError);
        Com<capture::IDirect3D11CaptureFramePool> pool;
        check(pools->CreateFreeThreaded(device.value, pixelFormat, 1, size, pool.out()), "Window frame pool",
              captureError);
        Com<capture::IGraphicsCaptureSession> session;
        check(pool->CreateCaptureSession(item.value, session.out()), "Window capture session", captureError);
        const struct Close { // The session and its pool end with the picture: so does the border.
            Com<capture::IGraphicsCaptureSession>& session;
            Com<capture::IDirect3D11CaptureFramePool>& pool;
            ~Close() {
                for (IUnknown* object : {static_cast<IUnknown*>(session.value), static_cast<IUnknown*>(pool.value)}) {
                    Com<ABI::Windows::Foundation::IClosable> closable;
                    if (SUCCEEDED(object->QueryInterface(__uuidof(ABI::Windows::Foundation::IClosable),
                                                         reinterpret_cast<void**>(closable.out())))) {
                        closable->Close();
                    }
                }
            }
        } close{session, pool};
        if (const auto cursor = query<capture::IGraphicsCaptureSession2>(session)) {
            cursor->put_IsCursorCaptureEnabled(false);
        }
        if (const auto border = query<capture::IGraphicsCaptureSession3>(session)) {
            border->put_IsBorderRequired(false); // Where the system lets it (Windows 11).
        }
        check(session->StartCapture(), "Starting window capture", captureError);
        SurfaceShader shader;
        for (;;) {
            Com<capture::IDirect3D11CaptureFrame> frame;
            if (SUCCEEDED(pool->TryGetNextFrame(frame.out())) && frame) {
                SizeInt32 content{};
                check(frame->get_ContentSize(&content), "Window frame size", captureError);
                Com<direct3d::IDirect3DSurface> surface;
                check(frame->get_Surface(surface.out()), "Window frame", captureError);
                auto access = surface.as<IDirect3DDxgiInterfaceAccess>("Window frame texture", captureError);
                Com<ID3D11Texture2D> texture;
                check(access->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture.out())),
                      "Window frame texture", captureError);
                D3D11_TEXTURE2D_DESC desc{};
                texture->GetDesc(&desc);
                if (content.Width > LONG(desc.Width) || content.Height > LONG(desc.Height)) {
                    // It grew since the pool was made: the next frame, in a pool of its size, shows all of it.
                    frame.reset();
                    check(pool->Recreate(device.value, pixelFormat, 1, content), "Window frame pool", captureError);
                } else if (content.Width > 0 && content.Height > 0) {
                    return drawThumbnail(shader, d3d, texture.value, 0, {0, 0, content.Width, content.Height}, 0,
                                         nullptr, request.maxWidth, request.maxHeight);
                }
            }
            if (Clock::now() >= deadline) {
                fail(RCS_ERROR_TIMEOUT, "Windows delivered no picture of the window within three seconds");
            }
            sleepPrecise(2ms);
        }
    });
}

} // namespace rcsdk::platform::win
