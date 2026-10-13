// Native Windows viewer: renders GPU frames with D3D11 on the SDK's device and
// forwards input (F8 toggles control). Hardware decoding only; the session
// fails to start on systems that cannot decode the offered codecs on the GPU.
//
//   rcsdk_windows_viewer --address 192.168.1.10 --key-file key.bin [--port 47990] [--codec h264|hevc|hevc,h264]
//   F8 control   F9 fit / 1:1   F11 full screen
#include "input.hpp"

#include <rcsdk/rcsdk.hpp>
#include <rcsdk/view_geometry.hpp>

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <fstream>
#include <mutex>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT frameMessage = WM_APP + 1;

// Latest frame handed from SDK threads to the UI thread.
struct Mailbox {
    std::mutex mutex;
    rcsdk::GpuFrame frame;
    bool posted = false;
    uint64_t frames = 0;
};

// Draws NV12/BGRA textures into a flip-model swap chain with the video processor,
// which also scales and converts colour. Uses the SDK's device so no copy is needed.
class Renderer {
public:
    bool draw(HWND window, const rcs_gpu_frame_info& frame, const rcsdk::ViewGeometry& view) {
        auto* device = static_cast<ID3D11Device*>(frame.device);
        if (device != device_.Get()) {
            reset(device);
        }
        RECT client{};
        GetClientRect(window, &client);
        const UINT width = UINT(std::max<LONG>(client.right, 1)), height = UINT(std::max<LONG>(client.bottom, 1));
        ComPtr<ID3D11Multithread> lock;
        context_.As(&lock);
        lock->Enter(); // The SDK uses the same immediate context on its own threads.
        const bool ok = present(window, frame, view, width, height);
        lock->Leave();
        return ok && SUCCEEDED(swapChain_->Present(0, 0));
    }

private:
    void reset(ID3D11Device* device) {
        *this = Renderer();
        device_ = device;
        device_->GetImmediateContext(&context_);
        device_.As(&video_);
        context_.As(&videoContext_);
    }

    bool present(HWND window, const rcs_gpu_frame_info& frame, const rcsdk::ViewGeometry& view, UINT width,
                 UINT height) {
        if (!swapChain_) {
            ComPtr<IDXGIDevice> dxgi;
            ComPtr<IDXGIAdapter> adapter;
            ComPtr<IDXGIFactory2> factory;
            device_.As(&dxgi);
            dxgi->GetAdapter(&adapter);
            adapter->GetParent(IID_PPV_ARGS(&factory));
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), window, &desc, nullptr, nullptr, &swapChain_))) {
                return false;
            }
        }
        DXGI_SWAP_CHAIN_DESC1 current{};
        swapChain_->GetDesc1(&current);
        if (current.Width != width || current.Height != height) {
            swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        }
        if (!processor_ || inputWidth_ != frame.surface_width || inputHeight_ != frame.surface_height ||
            outputWidth_ != width || outputHeight_ != height) {
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
            content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            content.InputWidth = frame.surface_width;
            content.InputHeight = frame.surface_height;
            content.OutputWidth = width;
            content.OutputHeight = height;
            content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            if (FAILED(video_->CreateVideoProcessorEnumerator(&content, &enumerator_)) ||
                FAILED(video_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_))) {
                return false;
            }
            inputWidth_ = frame.surface_width;
            inputHeight_ = frame.surface_height;
            outputWidth_ = width;
            outputHeight_ = height;
        }
        ComPtr<ID3D11Texture2D> backBuffer;
        swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
        inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
        outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorInputView> input;
        ComPtr<ID3D11VideoProcessorOutputView> output;
        if (FAILED(video_->CreateVideoProcessorInputView(static_cast<ID3D11Resource*>(frame.resource),
                                                         enumerator_.Get(), &inputDesc, &input)) ||
            FAILED(video_->CreateVideoProcessorOutputView(backBuffer.Get(), enumerator_.Get(), &outputDesc, &output))) {
            return false;
        }
        const RECT source{LONG(view.sourceX), LONG(view.sourceY), LONG(view.sourceX + view.sourceWidth),
                          LONG(view.sourceY + view.sourceHeight)};
        const RECT target{view.x, view.y, view.x + view.width, view.y + view.height};
        const RECT whole{0, 0, LONG(width), LONG(height)};
        videoContext_->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        videoContext_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &source);
        videoContext_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &target);
        videoContext_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &whole);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, out{};
        in.YCbCr_Matrix = frame.color.matrix == RCS_MATRIX_BT601 ? 0 : 1;
        in.Nominal_Range = frame.color.range == RCS_RANGE_LIMITED ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235
                                                                  : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        videoContext_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &in);
        videoContext_->VideoProcessorSetOutputColorSpace(processor_.Get(), &out);
        D3D11_VIDEO_COLOR black{};
        black.RGBA.A = 1;
        videoContext_->VideoProcessorSetOutputBackgroundColor(processor_.Get(), FALSE, &black);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = input.Get();
        return SUCCEEDED(videoContext_->VideoProcessorBlt(processor_.Get(), output.Get(), 0, 1, &stream));
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoDevice> video_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    ComPtr<IDXGISwapChain1> swapChain_;
    UINT inputWidth_ = 0, inputHeight_ = 0, outputWidth_ = 0, outputHeight_ = 0;
};

struct App {
    rcsdk::Session* session = nullptr;
    Mailbox mailbox;
    Renderer renderer;
    viewer::InputForwarder input;
    rcsdk::GpuFrame shown;
    uint32_t shownWidth = 0, shownHeight = 0;
    rcsdk::ViewScale scale = rcsdk::ViewScale::fit;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    uint64_t lastFrames = 0;
};

rcsdk::ViewGeometry geometry(HWND window, const App& app) {
    RECT client{};
    GetClientRect(window, &client);
    return rcsdk::viewGeometry(client.right, client.bottom, app.shownWidth, app.shownHeight, app.scale);
}

void paint(HWND window, App& app) {
    if (app.shown) {
        app.renderer.draw(window, app.shown.info(), geometry(window, app));
        ValidateRect(window, nullptr);
        return;
    }
    PAINTSTRUCT paint{}; // Black until the first frame arrives.
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    EndPaint(window, &paint);
}

void toggleFullScreen(HWND window, App& app) {
    const LONG style = GetWindowLongW(window, GWL_STYLE);
    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO monitor{sizeof(monitor)};
        GetWindowPlacement(window, &app.placement);
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        SetWindowLongW(window, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        const auto& r = monitor.rcMonitor;
        SetWindowPos(window, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(window, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(window, &app.placement);
        SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}

void updateTitle(HWND window, App& app) {
    uint64_t frames;
    {
        std::lock_guard lock(app.mailbox.mutex);
        frames = app.mailbox.frames;
    }
    const auto stream = app.session->streamInfo();
    const auto control = app.session->controlInfo();
    const auto title = std::to_wstring(stream.width) + L"x" + std::to_wstring(stream.height) + L"  " +
                       std::to_wstring(frames - app.lastFrames) + L" fps  " +
                       (app.input.enabled() ? L"[control on - F8]"
                        : control.granted   ? L"[F8: control]"
                                            : L"[view only]");
    app.lastFrames = frames;
    SetWindowTextW(window, (L"Remote Control SDK viewer  " + title).c_str());
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        return DefWindowProcW(window, message, w, l);
    }
    if (!app || !app->session) { // Messages sent while the window is being created.
        return DefWindowProcW(window, message, w, l);
    }
    if (app->input.handle(window, message, w, l, *app->session, geometry(window, *app), app->shownWidth,
                          app->shownHeight)) {
        return 0;
    }
    switch (message) {
    case frameMessage: {
        std::lock_guard lock(app->mailbox.mutex);
        app->mailbox.posted = false;
        if (app->mailbox.frame) {
            app->shown = std::move(app->mailbox.frame);
            app->mailbox.frame = {};
            const auto info = app->shown.info();
            app->shownWidth = info.width;
            app->shownHeight = info.height;
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_PAINT:
        paint(window, *app);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        updateTitle(window, *app);
        return 0;
    case WM_KEYDOWN:
        if (w == VK_F9) {
            app->scale = app->scale == rcsdk::ViewScale::fit ? rcsdk::ViewScale::nativePixels : rcsdk::ViewScale::fit;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (w == VK_F11) {
            toggleFullScreen(window, *app);
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}

std::string argument(int argc, char** argv, const std::string& name, const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) {
            return argv[i + 1];
        }
    }
    return fallback;
}

} // namespace

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    rcsdk::SessionConfig config(RCS_ROLE_VIEW);
    config.setAddress(argument(argc, argv, "--address", "127.0.0.1"),
                      uint32_t(std::stoul(argument(argc, argv, "--port", "47990"))));
    const std::string codec = argument(argc, argv, "--codec", "h264");
    if (codec == "hevc") {
        config.setCodecs({RCS_CODEC_HEVC});
    } else if (codec == "hevc,h264") {
        config.setCodecs({RCS_CODEC_HEVC, RCS_CODEC_H264});
    } else if (codec != "h264") {
        MessageBoxW(nullptr, L"--codec takes h264, hevc or hevc,h264.", L"Viewer", MB_ICONERROR);
        return 2;
    }
    std::ifstream key(argument(argc, argv, "--key-file", "key.bin"), std::ios::binary);
    auto& identity = config.core().identity;
    if (!key.read(reinterpret_cast<char*>(identity.key), sizeof(identity.key))) {
        MessageBoxW(nullptr, L"Pass --key-file with the 32-byte connection key.", L"Viewer", MB_ICONERROR);
        return 2;
    }

    App app;
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"RcsdkViewer";
    RegisterClassW(&windowClass);
    HWND window = CreateWindowW(windowClass.lpszClassName, L"Remote Control SDK viewer", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1600, 900, nullptr, nullptr, windowClass.hInstance, &app);

    const auto post = [&app, window] {
        if (!app.mailbox.posted) {
            app.mailbox.posted = true;
            PostMessageW(window, frameMessage, 0, 0);
        }
    };
    rcsdk::Callbacks callbacks;
    callbacks.onGpuFrame = [&app, post](const rcsdk::GpuFrame& frame) {
        std::lock_guard lock(app.mailbox.mutex);
        app.mailbox.frame = frame;
        ++app.mailbox.frames;
        post();
    };
    callbacks.onEvent = [window](const rcs_event& event) {
        const bool ended = event.type == RCS_EVENT_STATE &&
                           (event.state.value == RCS_STATE_FAILED || event.state.value == RCS_STATE_STOPPED);
        if (ended) {
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
    };
    rcsdk::Session session(config, std::move(callbacks));
    app.session = &session;
    if (session.start() != RCS_OK) {
        MessageBoxA(nullptr, ("Could not start the session: " + session.failure().message).c_str(), "Viewer", MB_ICONERROR);
        return 1;
    }
    ShowWindow(window, SW_SHOW);
    SetTimer(window, 1, 1000, nullptr);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    const std::string error = session.failure().message;
    app.input.disable(session);
    session.stop();
    if (!error.empty()) {
        MessageBoxA(nullptr, error.c_str(), "Viewer", MB_ICONERROR);
    }
    return error.empty() ? 0 : 1;
}
