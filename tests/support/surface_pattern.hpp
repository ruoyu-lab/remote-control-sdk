// A capture whose pictures are native GPU surfaces of one colour, as a
// display capture's are: CVPixelBuffers (BGRA, IOSurface backed) on macOS,
// BGRA textures of a hardware D3D11 device on Windows. Elsewhere there are
// none (available() is false).
#pragma once

#include "platform/platform.hpp"
#include "seams/seams.hpp"

#include <atomic>
#include <thread>

#ifdef __APPLE__
#include <CoreVideo/CoreVideo.h>
#elif defined(_WIN32)
#include <d3d11_4.h>
#endif

namespace rcsdk::test {

class SurfacePatternCapture final : public platform::ScreenCapture {
public:
    // The pictures' colour, B, G, R, A from the low byte up.
    static std::atomic<uint32_t>& colour() {
        static std::atomic<uint32_t> value{0xFF2040C0u};
        return value;
    }

    static bool available() {
#if defined(__APPLE__) || defined(_WIN32)
        return true;
#else
        return false;
#endif
    }

    void start(const platform::CaptureConfig& config, CancelToken) override {
        config_ = config;
        next_ = Clock::now();
#ifdef _WIN32
        ID3D11Device* created = nullptr;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                     D3D11_SDK_VERSION, &created, nullptr, nullptr))) {
            fail(RCS_ERROR_CAPTURE, "No D3D11 device for the test's surfaces");
        }
        ID3D11Multithread* multithread = nullptr;
        if (SUCCEEDED(created->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&multithread)))) {
            multithread->SetMultithreadProtected(TRUE);
            multithread->Release();
        }
        device_ = std::shared_ptr<void>(created, [](void* d) { static_cast<ID3D11Device*>(d)->Release(); });
#endif
    }

    bool next(VideoFrame& frame, std::chrono::milliseconds timeout, CancelToken) override {
        const auto now = Clock::now();
        if (now < next_) {
            std::this_thread::sleep_for(std::min<Clock::duration>(next_ - now, timeout));
            if (Clock::now() < next_) {
                return false;
            }
        }
        next_ += std::chrono::nanoseconds(1'000'000'000 / config_.framesPerSecond);
        frame = VideoFrame{};
        frame.width = config_.width;
        frame.height = config_.height;
        frame.timestampNs = monotonicNs();
        frame.surface = surface();
        frame.device = device_;
        ++captured_;
        return frame.surface != nullptr;
    }

    void setCursorInVideo(bool) override {}
    CursorState cursor() const override { return {}; }
    void stop() noexcept override {}
    uint64_t captured() const noexcept override { return captured_; }
    uint64_t overwritten() const noexcept override { return 0; }
    Rect content() const noexcept override { return {0, 0, config_.width, config_.height}; }
    platform::DisplayMode display() const noexcept override { return {1920, 1080, 60}; }
    std::shared_ptr<void> device() const noexcept override { return device_; }

private:
    std::shared_ptr<void> surface() const {
        const uint32_t pixel = colour();
#ifdef __APPLE__
        const void* keys[] = {kCVPixelBufferIOSurfacePropertiesKey};
        CFDictionaryRef empty = CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
        const void* values[] = {empty};
        CFDictionaryRef attributes = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                        &kCFTypeDictionaryValueCallBacks);
        CVPixelBufferRef buffer = nullptr;
        CVPixelBufferCreate(nullptr, config_.width, config_.height, kCVPixelFormatType_32BGRA, attributes, &buffer);
        CFRelease(attributes);
        CFRelease(empty);
        if (!buffer) {
            return {};
        }
        CVPixelBufferLockBaseAddress(buffer, 0);
        auto* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
        const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
        for (uint32_t y = 0; y < config_.height; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(base + y * stride);
            std::fill(row, row + config_.width, pixel);
        }
        CVPixelBufferUnlockBaseAddress(buffer, 0);
        return std::shared_ptr<void>(buffer, [](void* b) { CVPixelBufferRelease(static_cast<CVPixelBufferRef>(b)); });
#elif defined(_WIN32)
        std::vector<uint32_t> pixels(size_t(config_.width) * config_.height, pixel);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = config_.width;
        desc.Height = config_.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        D3D11_SUBRESOURCE_DATA data{pixels.data(), config_.width * 4, 0};
        ID3D11Texture2D* texture = nullptr;
        static_cast<ID3D11Device*>(device_.get())->CreateTexture2D(&desc, &data, &texture);
        if (!texture) {
            return {};
        }
        return std::shared_ptr<void>(texture, [](void* t) { static_cast<ID3D11Texture2D*>(t)->Release(); });
#else
        (void)pixel;
        return {};
#endif
    }

    platform::CaptureConfig config_;
    Clock::time_point next_;
    std::shared_ptr<void> device_;
    uint64_t captured_ = 0;
};

// Surfaces for every capture while it lives.
struct UseSurfacePattern {
    UseSurfacePattern() {
        seams::setCaptureFactory([](const auto&) { return std::make_unique<SurfacePatternCapture>(); });
    }
    ~UseSurfacePattern() { seams::setCaptureFactory({}); }
    UseSurfacePattern(const UseSurfacePattern&) = delete;
    UseSurfacePattern& operator=(const UseSurfacePattern&) = delete;
};

} // namespace rcsdk::test
