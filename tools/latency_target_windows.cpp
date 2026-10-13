// Full-screen latency target for Windows: a moving bar redrawn every compositor
// frame (so capture always has new content) and the visual marker, which
// advances when Space is pressed. Closes after the given number of seconds.
//
//   rcsdk_latency_target [seconds=60] [display=0] [noise] [burst] [clock=IP:PORT]
// "noise" fills the screen around the marker with random blocks that change
// every frame, so the encoder runs at its full bitrate (transport stress).
// display uses the SDK's numbering (1-based, monitor enumeration order); 0 is
// the primary display.
// "clock" makes the marker show the viewing machine's clock instead (its
// milliseconds modulo 256, redrawn every frame), synchronised over UDP with
// rcsdk_glass_probe there. The probe reads the marker from the screen, so
// it measures any remote desktop software end to end, glass to glass.
// "burst" keeps the whole screen still for 1.5 s, then changes it every frame
// for 0.5 s, repeatedly: what a viewer sees when something starts to move.
#include "visual_marker.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>

#include <chrono>
#include <string>
#include <vector>

namespace {
unsigned counter = 0;

LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_KEYDOWN && w == VK_SPACE && !(l & (1 << 30))) {
        counter = (counter + 1) & 255;
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
// Bounds of display `index` as numbered by the SDK; the primary display for 0.
RECT displayBounds(int index) {
    struct Search {
        int wanted, seen = 0;
        RECT bounds{};
    } search{index};
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(data);
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(monitor, &info)) {
                return TRUE;
            }
            ++s.seen;
            const bool match = s.wanted ? s.seen == s.wanted : (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
            if (match) {
                s.bounds = info.rcMonitor;
            }
            return !match;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.bounds;
}

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The viewing machine's clock: asks the probe for its time a few times a
// second and keeps the offset from the exchange with the shortest round trip
// among the recent ones (its error is at most half that round trip).
class RemoteClock {
public:
    explicit RemoteClock(const std::string& address) {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
        const auto colon = address.rfind(':');
        sockaddr_in peer{};
        peer.sin_family = AF_INET;
        peer.sin_port = htons(u_short(std::stoi(address.substr(colon + 1))));
        inet_pton(AF_INET, address.substr(0, colon).c_str(), &peer.sin_addr);
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        connect(socket_, reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
        u_long nonBlocking = 1;
        ioctlsocket(socket_, FIONBIO, &nonBlocking);
    }

    void poll() {
        const int64_t now = nowNs();
        if (now >= nextRequest_) {
            send(socket_, reinterpret_cast<const char*>(&now), sizeof(now), 0);
            nextRequest_ = now + 100'000'000;
        }
        int64_t reply[2];
        while (recv(socket_, reinterpret_cast<char*>(reply), sizeof(reply), 0) == int(sizeof(reply))) {
            const int64_t received = nowNs(), roundTrip = received - reply[0];
            samples_.push_back({roundTrip, reply[1] - (reply[0] + received) / 2});
            if (samples_.size() > 50) {
                samples_.pop_front();
            }
            best_ = *std::min_element(samples_.begin(), samples_.end(),
                                      [](const Sample& a, const Sample& b) { return a.roundTrip < b.roundTrip; });
        }
    }
    bool synced() const { return !samples_.empty(); }
    int64_t remoteNs() const { return nowNs() + best_.offset; }

private:
    struct Sample {
        int64_t roundTrip = 0, offset = 0;
    };
    SOCKET socket_;
    int64_t nextRequest_ = 0;
    std::deque<Sample> samples_;
    Sample best_;
};

} // namespace

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::stoi(argv[1]) : 60;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const RECT bounds = displayBounds(argc > 2 ? std::stoi(argv[2]) : 0);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = procedure;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"RcsdkLatencyTarget";
    RegisterClassW(&windowClass);
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    HWND window =
        CreateWindowExW(WS_EX_TOPMOST, windowClass.lpszClassName, L"SDK latency target", WS_POPUP | WS_VISIBLE,
                        bounds.left, bounds.top, width, height, nullptr, nullptr, windowClass.hInstance, nullptr);
    SetForegroundWindow(window);
    SetFocus(window);

    HBRUSH background = CreateSolidBrush(RGB(0x20, 0x2d, 0x42)), white = CreateSolidBrush(RGB(255, 255, 255));
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0)), cyan = CreateSolidBrush(RGB(0, 255, 255));
    {
        HDC dc = GetDC(window);
        const RECT all{0, 0, width, height};
        FillRect(dc, &all, background);
        ReleaseDC(window, dc);
    }
    bool noise = false, burst = false;
    std::unique_ptr<RemoteClock> clock;
    for (int i = 3; i < argc; ++i) {
        const std::string argument = argv[i];
        noise |= argument == "noise";
        burst |= argument == "burst";
        if (argument.rfind("clock=", 0) == 0) {
            clock = std::make_unique<RemoteClock>(argument.substr(6));
        }
    }
    constexpr int noiseWidth = 320, noiseHeight = 90;
    std::vector<uint32_t> pixels(size_t(noiseWidth) * noiseHeight);
    uint32_t state = 0x9e3779b9;
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(bitmap.bmiHeader);
    bitmap.bmiHeader.biWidth = noiseWidth;
    bitmap.bmiHeader.biHeight = -noiseHeight;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    unsigned drawn = 999, tick = 0;
    while (std::chrono::steady_clock::now() < end) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                return 0;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        const auto sinceStart = std::chrono::steady_clock::now() - (end - std::chrono::seconds(seconds));
        if (burst && sinceStart % std::chrono::milliseconds(2000) < std::chrono::milliseconds(1500)) {
            if (clock) {
                clock->poll();
            }
            DwmFlush(); // Still: draw nothing.
            continue;
        }
        if (clock) {
            clock->poll();
            counter = clock->synced() ? unsigned(clock->remoteNs() / 1'000'000) & 255 : 0;
        }
        HDC dc = GetDC(window);
        if (drawn != counter) {
            const uint32_t value = marker::bits(uint8_t(counter));
            for (int i = 0; i < 32; ++i) {
                const RECT bar{int(width * (.2 + i * .6 / 32)), int(height * .42),
                               int(width * (.2 + (i + 1) * .6 / 32)), int(height * .58)};
                FillRect(dc, &bar, (value & (1u << (31 - i))) ? white : black);
            }
            drawn = counter;
        }
        if (noise) { // Above and below the marker, never over it.
            for (auto& pixel : pixels) {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                pixel = state & 0xffffff;
            }
            StretchDIBits(dc, 0, 0, width, int(height * .4), 0, 0, noiseWidth, noiseHeight, pixels.data(), &bitmap,
                          DIB_RGB_COLORS, SRCCOPY);
            StretchDIBits(dc, 0, int(height * .6), width, int(height * .4), 0, 0, noiseWidth, noiseHeight,
                          pixels.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
        }
        const RECT strip{0, int(height * .8), width, int(height * .8) + 40};
        FillRect(dc, &strip, background);
        const int x = int((tick++ * 13) % unsigned(std::max(1, width - 120)));
        const RECT moving{x, strip.top, x + 120, strip.bottom};
        FillRect(dc, &moving, cyan);
        ReleaseDC(window, dc);
        DwmFlush(); // One update per compositor frame.
    }
    DestroyWindow(window);
    return 0;
}
