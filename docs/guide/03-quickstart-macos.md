# 03 快速开始：macOS

## 要求

- macOS 14 或更新（ScreenCaptureKit）；构建出的库最低要求 macOS 14.0，与构建机的系统版本无关。Apple 芯片和 Intel 都行（Intel 版用预设 `macos-x86_64`）。
- Xcode 命令行工具，CMake 3.25 起，Ninja（用预设时）。
- 硬件编解码：H.264、HEVC（所有 Apple 芯片），VP9 只能解码（Apple 芯片），AV1 只能解码（M3 起）。VP8、VP9、AV1 的编码和 VP8、AV1 的软件解码用 SDK 自带的库（libvpx、libaom、dav1d），用 `RCS_ACCELERATION_SOFTWARE` 或 `HARDWARE_FIRST`。

## 引入 SDK

**作为 CMake 子项目**（推荐，和宿主一起编译；作为子项目时示例、工具和测试默认都不编译）：

```cmake
add_subdirectory(third_party/remote-control-sdk)
target_link_libraries(my_app PRIVATE RemoteControl::Core)        # 核心
target_link_libraries(my_app PRIVATE RemoteControl::Clipboard)   # 要哪个官方模块链哪个（15）
```

**安装后的包**：

```sh
cmake --preset release && cmake --build --preset release
cmake --install build/release --prefix /opt/rcsdk
```

```cmake
find_package(RemoteControlSDK 0.1 REQUIRED)   # 1.0 之前只匹配同一个次版本
target_link_libraries(my_app PRIVATE RemoteControl::Core)
```

- 默认编成动态库：`librcsdk_core.0.1.dylib`（按次版本命名，装错版本时系统加载就失败）。`-DBUILD_SHARED_LIBS=OFF` 编静态库，要链接的系统框架经 CMake 自动传给宿主。
- `RemoteControl::All` 是核心加全部构建了的官方模块和组件；`RCSDK_MODULES`、`RCSDK_COMPONENTS` 选构建哪些（默认 `all`）。
- 头文件和库必须是同一个版本：动态库放在应用包里时，启动时比较 `rcs_version_number()` 和 `RCSDK_VERSION`（[21](21-versions-distribution.md)）。

## 系统权限

| 用途 | 系统设置里的名字 | 查询 / 申请 |
| --- | --- | --- |
| 分享屏幕和声音 | “屏幕与系统音频录制”（屏幕和声音共用） | `rcs_screen_capture_permission` / `rcs_request_screen_capture_permission` |
| 执行远程输入（分享端，`RCS_INPUT_SYSTEM`） | “辅助功能” | `rcs_input_permission` / `rcs_request_input_permission` |
| 麦克风（`RCS_AUDIO_MICROPHONE`） | “麦克风”；应用的 `Info.plist` 要有 `NSMicrophoneUsageDescription` | `rcs_microphone_permission` / `rcs_request_microphone_permission` |

- 申请函数要在主线程调用（否则 `RCS_ERROR_WRONG_THREAD`），第一次会弹系统的提示；之后只能在“系统设置 › 隐私与安全性”里改。没有权限时：开始分享返回 `RCS_ERROR_PERMISSION_REQUIRED`；允许控制（`rcs_control_config.allowed`）同样被拒绝；麦克风那个方向暂停并报告原因，授权后自动开始。
- macOS 按应用的**代码签名**记录这些授权：开发期间每次重新签名都可能要重新授权。开发时可以用 ad-hoc 签名但固定指定要求（designated requirement）为包标识，示例的 CMake 函数 `rcsdk_sign_for_development`（`examples/CMakeLists.txt`）就是这样做的；发布时用 Developer ID 签名。
- 硬化运行时（hardened runtime）下录麦克风要 `com.apple.security.device.audio-input` 权利；沙盒应用连网要 `com.apple.security.network.client` 和（分享端监听时）`network.server`。SDK 没有在沙盒应用里验证过。
- 没有界面的宿主（命令行、守护进程）要让主线程的 run loop 转着（[01](01-concepts.md) 的示例用 `CFRunLoopRunInMode`）。

## 第一个分享端和观看端

[01](01-concepts.md#最小的完整程序) 的 `sharer.c` / `viewer.c`（或 C++ 版本）原样在 Mac 上编译运行，链接 `RemoteControl::Core` 和 `-framework CoreFoundation`。另一端可以是另一台 Mac、Windows，或者示例 `rcsdk_cli`：

```sh
./build/release/bin/rcsdk_cli --generate-key key.bin
./build/release/bin/rcsdk_cli --role share --address 192.168.1.10 --key-file key.bin --allow-control
./viewer 192.168.1.10 key.bin
```

## 观看端：用 Metal 显示画面

`on_gpu_frame` 交来的是 `CVPixelBufferRef`（IOSurface 支撑、兼容 Metal），解码帧默认是 NV12（`420v` / `420f`）；10 位是 P010（`x420`），4:4:4 是 NV24（`444v`）和 P410（`x444`），按 `rcs_gpu_frame_info.format` 区分。用 `CVMetalTextureCache` 零复制映射成两个平面的纹理，在着色器里按 `color.matrix` 和 `color.range` 转 RGB：

```objc
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#include <rcsdk/gpu.h>

// 一次：CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, device, nil, &cache);
// 每帧，在渲染线程上（帧已经 rcs_gpu_frame_retain）：把 CVPixelBuffer 的两个平面映射成 Metal 纹理，不复制。
static bool mapPlanes(CVMetalTextureCacheRef cache, rcs_gpu_frame frame, CVMetalTextureRef planes[2],
                      rcs_gpu_frame_info* info) {
    *info = (rcs_gpu_frame_info)RCS_GPU_FRAME_INFO_INIT;
    if (rcs_gpu_frame_get_info(frame, info) != RCS_OK || info->resource_type != RCS_GPU_CVPIXELBUFFER) {
        return false;
    }
    CVPixelBufferRef buffer = (CVPixelBufferRef)info->resource;
    const bool tenBit = info->format == RCS_PIXEL_P010 || info->format == RCS_PIXEL_P410;
    const MTLPixelFormat formats[2] = {tenBit ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm,
                                       tenBit ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm};
    for (size_t plane = 0; plane < 2; ++plane) {   // NV12 / P010：色度平面是一半大小；NV24 / P410：全尺寸
        if (CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, cache, buffer, nil, formats[plane],
                                                      CVPixelBufferGetWidthOfPlane(buffer, plane),
                                                      CVPixelBufferGetHeightOfPlane(buffer, plane), plane,
                                                      &planes[plane]) != kCVReturnSuccess) {
            return false;
        }
    }
    // CVMetalTextureGetTexture(planes[0]) / (planes[1]) 交给着色器，按 info->color.matrix 和 info->color.range
    // 把 YCbCr 转成 RGB；只画 info->width × info->height（表面可能更大）。GPU 用完后 CFRelease 两个
    // CVMetalTextureRef，再 rcs_gpu_frame_release(frame)。
    return true;
}
```

- 帧到了就画、画完立即提交（`CAMetalLayer.displaySyncEnabled = NO`），窗口由系统合成器同步，不会撕裂；等 vsync 在 120 FPS 的流、120 Hz 的屏上会让延迟翻倍（[08](08-viewing.md#怎样画得快)）。
- 只想要 CPU 图像的宿主注册 `on_frame`：VideoToolbox 直接转好 BGRA，读取时零复制映射。
- 完整的 Qt Quick 写法见 `examples/qml-demo`（`native_planes_metal.mm`、`video_node_rhi.cpp`、`remote_video.cpp`）。

## 分享端在 macOS 上

- 采集：ScreenCaptureKit，显示器、区域、窗口、应用四种来源都有（[07](07-sharing.md)）。锁屏、显示器睡眠、改分辨率时采集暂停（`RCS_EVENT_CAPTURE`），之后自己恢复。
- 声音：默认用 Core Audio 进程捕获（macOS 14.2 起，每 5 ms 一块），14.0、14.1 上用 ScreenCaptureKit（每 20 ms 一块）；`rcs_audio_tuning_config.capture_backend` 可以指定。
- 编码：VideoToolbox 硬件编码，低延迟模式跟不上帧率时（例如 M4 Pro 上的 4K60）切到流水线编码器，每帧多约 15 ms，`RCS_EVENT_CODEC` 的 `low_latency` 说明。
- 远程输入：CoreGraphics 事件，要“辅助功能”权限；笔是数位板事件，Mac 没有触摸输入。
- Ctrl+Alt+Del 没有对应的东西（`UNSUPPORTED`）；跟随安全桌面只在 Windows 上有。
- 无人值守（登录前、注销后都能连）用 LaunchDaemon + LaunchAgent，见 [16](16-unattended.md#macos)。
