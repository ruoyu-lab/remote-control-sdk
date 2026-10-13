# 04 快速开始：Windows

## 要求

- Windows 10 1803 或更新（x64；ARM64 也能编）。只共享单个窗口要 1903 起（Windows.Graphics.Capture），触摸和笔的注入要 1809 起，TURN over TLS 用 Schannel（1809 起，Windows 11 起有 TLS 1.3）。系统做不到的在 `rcs_capabilities` 里不报告，用到时以 `UNSUPPORTED` 说明。
- HEVC 解码要装 Microsoft 的“HEVC 视频扩展”，AV1 解码要“AV1 视频扩展”，VP8 / VP9 解码要“VP9 视频扩展”（很多品牌机预装）；没有时能力里不报告，只列了 HEVC 的观看端 `start` 返回 `UNSUPPORTED`。有 oneVPL 运行时的 Intel 显卡上例外：没有这些扩展（或没有 Media Foundation）时，这些编码格式由显卡驱动带的 oneVPL 在 GPU 上解（还没有在没装扩展的机器上实测）。默认配置只用 H.264，不受影响。VP8、VP9、AV1 另有 SDK 自带的软件实现（libvpx、dav1d、libaom），不用装扩展。
- 从源码编：x64 上要 NASM（自带编解码库的汇编，[BUILDING](../BUILDING.md)）。
- Windows N / KN 版要装“媒体功能包”（Media Feature Pack），Windows Server 要装“Media Foundation”功能：没有 `mfplat.dll` 时 SDK 照常加载、运行，只是实现列表里没有 Media Foundation 的编解码器，能力里没有 AAC；用它们的以 `UNSUPPORTED` 失败，原因写明要装什么。NVENC、宿主自己的编解码器、Opus 和 PCM 声音、数据通道和远程控制不受影响。
- 硬件编码：NVIDIA 显卡上默认直接调 NVENC（驱动 522.25 起），Intel 显卡上默认直接调显卡驱动带的 oneVPL 运行时（Tiger Lake 起的显卡；更老的、没有这个运行时的照旧经 Media Foundation），其他显卡经 Media Foundation；`rcs_encoder_config.backend` 可以指定（[13](13-tuning.md)）。分享端用的是被共享的显示器所在的 GPU 的编码器。
- 混合显卡（笔记本常见：显示器接核显，独立显卡做计算）：被共享的显示器所在的 GPU 是 DXGI 把它挂在哪块 GPU 名下，这由 Windows 给程序的默认 GPU 决定（“设置 › 系统 › 屏幕 › 显示卡”可按程序选节能或高性能）：默认是核显时采集和编码都在核显上，默认是独显时 Windows 把核显的显示器画面跨显卡交给独显，用独显的编码器（台式机上实测过后一种，见 [PERFORMANCE](../PERFORMANCE.md) 的“Windows：混合显卡上的 Media Foundation 编码器”）。NVIDIA 的 Media Foundation 编码器只在 NVIDIA 显卡是默认 GPU 时起得来，否则不在实现列表里，指定它时以 `UNSUPPORTED` 失败并说明；NVENC（NVIDIA 上默认）不受影响。
- 硬件解码：Media Foundation（HEVC、VP9、AV1 要系统的视频扩展）；4:4:4 的 HEVC 在 NVIDIA 显卡上由 NVDEC、在 Intel 显卡上由 oneVPL 在 GPU 上解，Intel 显卡上 VP9 4:4:4 也由 oneVPL 解（观看端 `acceleration` 设 `HARDWARE_FIRST`，见 [08](08-viewing.md)），别的显卡（AMD）上只有 CPU 上的软件解码器解。Intel 显卡上 4:2:0 仍由 Media Foundation 解（同一块核显上每帧快 0.5～3 ms），`rcs_decoder_config.backend` 设 `RCS_BACKEND_ONEVPL` 可以指定 oneVPL。NVENC、NVDEC 和 oneVPL 都随显卡驱动装好，SDK 运行时加载，不需要另外安装。

## 引入 SDK

- **MSVC**：在 Visual Studio 开发者命令行里 `cmake --preset windows-msvc`、`cmake --build --preset windows-msvc`，得到动态库 `rcsdk_core.dll`（和各个模块的 DLL）。宿主用 CMake 时同样 `add_subdirectory` 或 `find_package(RemoteControlSDK 0.1)` 加 `RemoteControl::Core`。
- **在 macOS 上交叉编译**：预设 `windows-x64-release` / `windows-arm64-release`（LLVM-MinGW，静态库，产物只依赖系统 DLL），见 [BUILDING](../BUILDING.md)。
- 静态链接时要链接的系统库（`dwmapi`、`runtimeobject`、`version`、`secur32`、`crypt32` 等）经 CMake 的 `RemoteControl::Core` 自动带上。
- DLL 放在应用目录里时，启动时比较 `rcs_version_number()` 和 `RCSDK_VERSION`，不一样就别用（[21](21-versions-distribution.md)）。

## 权限和会话

- **采集屏幕的进程必须在已登录用户的交互式会话里**，不能是服务或会话 0（服务没有桌面）。服务可以持有会话，把采集交给用户会话里的助手进程（`rcs_agent_link_config`），见 [16](16-unattended.md)。
- **远程输入**：在用户会话里的进程就能注入（`rcs_input_permission` 为 `GRANTED`；会话 0 里是 `UNAVAILABLE`）。默认只在普通桌面上：锁屏、UAC 安全桌面出现时控制权暂停、按住的键被释放，之后自动恢复；以管理员身份运行的窗口不接受普通进程注入的输入。以 LocalSystem 运行并打开 `secure_desktop` 时，登录屏、锁屏、UAC 和管理员窗口都能看、能控制（[16](16-unattended.md)）。
- **麦克风**：“设置 › 隐私 › 麦克风”里允许桌面应用访问；SDK 只读这个设置，不弹提示。
- **防火墙**：Windows 防火墙按程序路径放行，程序第一次监听端口时弹确认框。分享端只用 P2P / 中继（只有出站连接）时不需要放行入站。

## 第一个分享端和观看端

[01](01-concepts.md#最小的完整程序) 的程序原样在 Windows 上编译运行（MSVC 编 C 文件要 `/utf-8` 才能正确处理中文注释和字符串）。另一端可以是 Mac、另一台 PC 或 `rcsdk_cli`。

## 观看端：用 D3D11 显示画面

`on_gpu_frame` 交来的是 `ID3D11Texture2D*`（`rcs_gpu_frame_info.resource`），它所在的设备是 `info.device`（`ID3D11Device*`）：

- 硬件解码的帧默认是 NV12，10 位是 P010；软件解码（`RCS_ACCELERATION_SOFTWARE`，或 `HARDWARE_FIRST` 时硬件解不了的格式，例如 HEVC 4:4:4）在 CPU 上解出、上传成 BGRA8 纹理交出。按 `info.format` 区分。
- NV12 纹理可以直接建两个着色器视图：`DXGI_FORMAT_R8_UNORM` 是亮度平面，`DXGI_FORMAT_R8G8_UNORM` 是色度平面（P010 用 `R16_UNORM` / `R16G16_UNORM`），在着色器里按 `info.color.matrix` 和 `info.color.range` 转 RGB。也可以像 `examples/windows/viewer.cpp` 那样用视频处理器（`ID3D11VideoProcessor`）直接画到交换链上。
- 默认帧在 SDK 自己创建的设备上：用 `info.device` 建视图，或者用显式的共享资源互通。**更好的做法**是把宿主渲染用的设备交给 SDK（`RCS_EXT_GPU_DEVICE`），SDK 就直接在这个设备上解码，不跨设备复制。这个设备必须用 `D3D11_CREATE_DEVICE_VIDEO_SUPPORT` 创建（提供 D3D11 视频接口）并打开多线程保护，否则 `start` 返回 `INVALID_ARGUMENT` 并说明缺了哪一项；这块 GPU 硬件解不了列出的任何格式时是 `UNSUPPORTED`。
- SDK 的解码线程也用这个设备的即时上下文：宿主自己的“设置状态 + 绘制”要用 `ID3D11Multithread::Enter` / `Leave` 锁住。
- 画完立即提交（`Present(0, 0)`），不要等 vsync（[08](08-viewing.md#怎样画得快)）。

下面的片段按头文件和 `examples/windows/viewer.cpp` 核对过，但没有在本文的编写环境里编译（需要 Windows SDK）：

```cpp
#include <d3d11_4.h>
#include <wrl/client.h>
#include <rcsdk/gpu.hpp>
#include <rcsdk/rcsdk.hpp>
using Microsoft::WRL::ComPtr;

// 宿主自己的设备，带视频接口和多线程保护，同时交给自己的渲染器和 SDK。
ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;
D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                  D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                  D3D11_SDK_VERSION, &device, nullptr, &context);
ComPtr<ID3D11Multithread> lock;
context.As(&lock);
lock->SetMultithreadProtected(TRUE);

auto config = rcsdk::SessionConfig(RCS_ROLE_VIEW).setAddress("192.168.1.10", 47990).setKey(key)
                  .use(rcsdk::gpuDevice(device.Get()));   // 在一次运行中固定

// 渲染线程，拿到一帧（rcsdk::GpuFrame 已经留住它）：
void draw(const rcsdk::GpuFrame& frame) {
    const rcs_gpu_frame_info info = frame.info();
    auto* texture = static_cast<ID3D11Texture2D*>(info.resource);
    const bool tenBit = info.format == RCS_PIXEL_P010;
    D3D11_SHADER_RESOURCE_VIEW_DESC luma{};
    luma.Format = tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    luma.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    luma.Texture2D.MipLevels = 1;
    D3D11_SHADER_RESOURCE_VIEW_DESC chroma = luma;
    chroma.Format = tenBit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    ComPtr<ID3D11ShaderResourceView> y, uv;
    device->CreateShaderResourceView(texture, &luma, &y);   // BGRA8（软件解码）只建一个 B8G8R8A8_UNORM 视图
    device->CreateShaderResourceView(texture, &chroma, &uv);
    lock->Enter();
    // ... 设置着色器（按 info.color 转 RGB）、绑定 y / uv、画 info.width × info.height 的部分 ...
    lock->Leave();
    // swapChain->Present(0, 0);
}
```

- **用 D3D12 渲染的宿主**：SDK 只交出 D3D11 纹理（或 CPU 帧），解码留在 D3D11 / Media Foundation 上（RTX 4090 上实测，交给 D3D12 多出的复制和栅栏测不出差别，见 [PERFORMANCE](../PERFORMANCE.md) “D3D12 解码值不值得”）。需要 D3D12 的宿主自己做 D3D11 和 D3D12 之间的共享纹理和栅栏，或者取 CPU 帧。
- 只要 CPU 图像的宿主注册 `on_frame`：硬件解码时由 GPU 的视频处理器转成 BGRA 再读回。

## 分享端在 Windows 上

- 采集：桌面复制（显示器、区域、应用），单个窗口用 Windows.Graphics.Capture（Windows 10 上被采集的窗口周围有系统画的黄框，Windows 11 起 SDK 把它关掉）。锁屏、UAC、改分辨率、开关 HDR 时采集暂停并自动恢复。HDR 桌面按“SDR 内容亮度”换算成 SDR。
- **一个进程同时只能复制一个显示器一次**：同一个进程里两次运行同时采集同一个显示器时，后开始的以 `RCS_ERROR_CAPTURE` 失败；要给几个人看同一个显示器，用一次运行加 `max_viewers`（[07](07-sharing.md#一对多)），或者放在不同的进程里。
- NVENC 在消费级显卡上同时能开的编码会话有限（现在的驱动是 8 个），几个分享会话、每个几路时要算上。
- NVENC 每种画面格式能编的每秒像素数有上限（它支持的最高 Level），列在 `rcs_codec_implementation.max_pixels_per_second`。RTX 4090（驱动 616.92）上，H.264 的 4:4:4 只到 Level 6.0（3840×2160 最多 128 帧；4:2:0 到 6.2），HEVC 各格式都到 6.2（4K 240 帧实测能编）。分享端的帧率超过它时，SDK 把帧率降到它能编的（尺寸不变，`rcs_stream_info.frames_per_second` 是实际的，日志写一行）；运行中仍然打不开时，这次改动不生效、流照旧（`RCS_EVENT_ENCODING`，`encoding` 0）。
- 声音：WASAPI 环回，每个系统混音周期（通常 10 ms）一块；排除本应用的声音要 Windows 10 build 20348 / Windows 11（进程环回）。
- 远程输入：`SendInput`（按键是扫描码），触摸和笔用合成指针设备（1809 起）。
- 无人值守（登录屏、锁屏、UAC 下也能用）：看护者和服务，见 [16](16-unattended.md#windows)。
