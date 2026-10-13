// COM, D3D11 and timing helpers for the Windows backend.
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi1_2.h>

#include "util/util.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace rcsdk::platform::win {

// Owns one COM reference.
template <class T>
class Com {
public:
    T* value = nullptr;
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    Com& operator=(Com&& other) noexcept {
        reset();
        value = std::exchange(other.value, nullptr);
        return *this;
    }
    T** out() noexcept {
        reset();
        return &value;
    }
    T* operator->() const noexcept { return value; }
    explicit operator bool() const noexcept { return value != nullptr; }
    void reset() noexcept {
        if (value) {
            value->Release();
            value = nullptr;
        }
    }
    // Takes a new reference to a borrowed pointer.
    void assign(T* borrowed) noexcept {
        if (borrowed) {
            borrowed->AddRef();
        }
        reset();
        value = borrowed;
    }
    template <class U>
    Com<U> as(const char* what, rcs_result category) const;
};

inline void check(HRESULT result, const char* operation, rcs_result category = RCS_ERROR_INTERNAL) {
    if (FAILED(result)) {
        char code[16];
        std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(result));
        fail(category, std::string(operation) + " failed (HRESULT " + code + ")");
    }
}

template <class T>
template <class U>
Com<U> Com<T>::as(const char* what, rcs_result category) const {
    Com<U> result;
    check(value->QueryInterface(__uuidof(U), reinterpret_cast<void**>(result.out())), what, category);
    return result;
}

// COM on this thread while it lives (a thread that has COM already keeps it as it is).
class ComThread {
public:
    ComThread() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComThread() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }
    ComThread(const ComThread&) = delete;
    ComThread& operator=(const ComThread&) = delete;

private:
    HRESULT result_;
};

// Shares ownership of a COM object through VideoFrame::surface/device.
template <class T>
std::shared_ptr<void> share(T* object) {
    object->AddRef();
    return {object, [](void* value) { static_cast<T*>(value)->Release(); }};
}

// Stage trace for performance work, built in with the CMake option
// RCSDK_TRACE (off by default). Lines are "<steady microseconds> <stage>" on
// stderr.
inline void trace([[maybe_unused]] const char* stage) {
#ifdef RCSDK_TRACE
    std::fprintf(stderr, "%llu %s\n", static_cast<unsigned long long>(monotonicNs() / 1000), stage);
#endif
}

// Per-monitor DPI awareness for the current thread, restored on exit.
struct DpiScope {
    DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ~DpiScope() {
        if (previous) {
            SetThreadDpiAwarenessContext(previous);
        }
    }
};

// Holds the device's multithread lock for a whole state-setting/draw sequence,
// since hosts may use the same immediate context from other threads.
class ContextLock {
public:
    explicit ContextLock(ID3D11DeviceContext* context) {
        context->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(lock_.out()));
        lock_->Enter();
    }
    ~ContextLock() { lock_->Leave(); }

private:
    Com<ID3D10Multithread> lock_;
};

struct Device {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;

    // Hardware device with video support and multithread protection.
    void create(IDXGIAdapter* adapter = nullptr) {
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        check(D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 2,
                                D3D11_SDK_VERSION, device.out(), nullptr, context.out()),
              "D3D11 device creation");
        context.as<ID3D10Multithread>("D3D11 multithread protection", RCS_ERROR_INTERNAL)
            ->SetMultithreadProtected(TRUE);
    }

    // A host's device. It is checked, never changed: the host keeps control
    // of its own device's settings.
    void adopt(ID3D11Device* host) {
        device.assign(host);
        device->GetImmediateContext(context.out());
        if (!context.as<ID3D10Multithread>("D3D11 multithread protection", RCS_ERROR_INVALID_ARGUMENT)
                 ->GetMultithreadProtected()) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "The host's D3D11 device needs multithread protection enabled");
        }
        Com<ID3D11VideoDevice> video;
        if (FAILED(device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(video.out())))) {
            fail(RCS_ERROR_INVALID_ARGUMENT,
                 "The host's D3D11 device has no video interfaces; create it with D3D11_CREATE_DEVICE_VIDEO_SUPPORT");
        }
    }
};

// Marks the GPU work submitted so far; the returned function waits for it.
inline std::function<rcs_result(uint32_t)> gpuFence(ID3D11Device* device, ID3D11DeviceContext* context) {
    Com<ID3D11Query> query;
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    check(device->CreateQuery(&desc, query.out()), "GPU fence");
    context->End(query.value);
    context->Flush();
    auto fence = share(query.value);
    auto owner = share(context);
    return [fence, owner](uint32_t timeoutMs) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        auto* context = static_cast<ID3D11DeviceContext*>(owner.get());
        for (;;) {
            BOOL done = FALSE;
            const HRESULT result = context->GetData(static_cast<ID3D11Query*>(fence.get()), &done, sizeof(done), 0);
            if (result == S_OK && done) {
                return RCS_OK;
            }
            if (FAILED(result)) {
                return RCS_ERROR_INTERNAL;
            }
            if (Clock::now() >= deadline) {
                return RCS_ERROR_TIMEOUT;
            }
            sleepPrecise(250us);
        }
    };
}

inline D3D11_TEXTURE2D_DESC textureDesc(uint32_t width, uint32_t height, DXGI_FORMAT format, UINT bind) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    return desc;
}

} // namespace rcsdk::platform::win
