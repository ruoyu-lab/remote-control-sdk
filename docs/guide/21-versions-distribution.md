# 21 版本、升级、分发与许可

## 版本规则（1.0 之前）

SDK 还在开发阶段，1.0 之前**不保留兼容**（理由见 [API-DESIGN 第 3 节](../API-DESIGN.md#3-版本规则10-之前)）：

- **每个版本都可能有破坏性修改**：结构体字段的顺序和名称、枚举的编号、默认值、网络协议都可能改变。升级时按 [CHANGELOG](../../CHANGELOG.md) 每个版本的“迁移”一节改代码并重新编译。
- **一个版本号**：`rcsdk.h` 的 `RCSDK_VERSION_MAJOR` / `MINOR` / `PATCH` 和 `RCSDK_VERSION`（`(MAJOR << 16) | (MINOR << 8) | PATCH`）是编译时的版本，`rcs_version_number()` 和 `rcs_version()`（例如 `"0.1.1-beta"`；正式发布的版本没有 `-` 后面的阶段，阶段在 `RCSDK_VERSION_LABEL`）是实际加载的库的版本。
- **头文件和库必须是同一个版本**，不同时明确报错，不去猜：
  - `RCS_SESSION_CONFIG_INIT` 把 `version` 填成 `RCSDK_VERSION`，与库不同时 `rcs_session_create` 返回 `INVALID_ARGUMENT`，原因里写明两个版本（助手的 `rcs_agent_config.version`、看护者的 `rcs_supervisor_config.version` 也一样）。
  - 每个结构体的 `struct_size`、列表函数的 `item_size` 必须正好是本版本的 `sizeof`；用 `*_INIT` 宏和 `sizeof` 就不会错。库从不按前缀读写。
  - 库在运行时才加载时（Windows 上放在应用目录里的 DLL、macOS 应用包里的 dylib），启动时检查 `rcs_version_number() == RCSDK_VERSION`。macOS 和 Linux 的动态库按次版本命名（`librcsdk_core.0.1.dylib`），装错版本时系统加载就失败；CMake 包只匹配同一个次版本（`find_package(RemoteControlSDK 0.1)`）。
- **两端运行同一个版本**：网络协议的版本在握手的第一条消息里（0.1.1-beta 是 v1），每改一次线上格式就加一。协议版本不同的两端握手失败：分享端告诉对方自己的协议版本、丢弃这个连接（计入 `rejected_connections`）；观看端以 `RCS_ERROR_UNSUPPORTED` 失败，原因里写明两个版本。宿主最好在自己的信令或配对流程里交换版本号，更早地提示用户升级。
- **助手链接**有自己的明文前缀（链接格式号，0.1.1-beta 是 1，加上 `RCSDK_VERSION`）：会话和另一个进程里的助手必须是同一个版本，不同时助手以 `UNSUPPORTED` 结束、不再重试。
- **模块**的线上格式靠模块自己的 `version`：两端不同时这个连接上不激活（`RCS_MODULE_INACTIVE`）。
- 1.0 起才承诺兼容，规则届时写进 API-DESIGN。

## 引入方式

| 方式 | 怎么做 |
| --- | --- |
| CMake 子项目（推荐） | `add_subdirectory(remote-control-sdk)`，`target_link_libraries(app PRIVATE RemoteControl::Core)`。作为子项目时示例、工具和测试都不编译 |
| 安装后的 CMake 包 | `cmake --install <构建目录> --prefix <目录>`，宿主 `find_package(RemoteControlSDK 0.1 REQUIRED)` |
| iOS | `scripts/build-xcframework.sh` 的 `RemoteControl.xcframework`（[05](05-quickstart-ios.md)） |
| Android | `scripts/build-android-aar.sh` 的 AAR（Prefab）（[06](06-quickstart-android.md)） |

库：`RemoteControl::Core`（核心）；官方模块 `RemoteControl::Clipboard`、`FileTransfer`、`Power`、`Thumbnail`、`TextCaret`；组件 `RemoteControl::Supervisor`（Windows 的看护者）；`RemoteControl::All` 是全部构建了的。CMake 选项（完整的见 [BUILDING](../BUILDING.md)）：`BUILD_SHARED_LIBS`（默认 ON；iOS、Android 默认静态）、`RCSDK_MODULES`、`RCSDK_COMPONENTS`、`RCSDK_CRYPTO_BACKEND`（`system`：Apple 系统和 Windows 的默认；`bearssl`：其他系统的默认）。静态链接时，要链接的系统库经 CMake 自动传给宿主。

## 分发时带上什么

| 系统 | 文件 |
| --- | --- |
| macOS | 动态库时：`librcsdk_core.0.1.dylib` 和用到的模块的 dylib（放进应用包、和应用一起签名）；静态库时什么都不用带 |
| Windows | 动态库时：`rcsdk_core.dll` 和用到的模块的 DLL，放在应用目录里；LLVM-MinGW 编的静态版只依赖系统 DLL。系统组件由用户的系统提供：HEVC / AV1 视频扩展、Media Foundation（N 版要装媒体功能包） |
| iOS | XCFramework 是静态的，编进应用；广播扩展也链接它 |
| Android | AAR 里的静态库编进宿主的原生库；arm64-v8a 和 x86_64（模拟器、Chromebook），APK 只带自己要的（`abiFilters`） |

- **代码签名**：macOS 的屏幕录制、辅助功能、麦克风授权按应用的代码签名记录，发布时用 Developer ID 签名并公证；硬化运行时下录麦克风要 `com.apple.security.device.audio-input` 权利。Windows 上没签名的程序（尤其是服务）在很多企业环境里会被拦。
- **防火墙**：分享端监听时（UDP、TCP）Windows 防火墙按程序路径放行；只用 P2P / 中继、宿主的通道时只有出站连接。
- **权限说明**：iOS 的 `Info.plist`（麦克风、本地网络），Android 的清单（[06](06-quickstart-android.md#权限清单)），macOS 的 `Info.plist`（麦克风）。

## 许可

SDK 本身是 **MIT 许可**（仓库根目录的 [LICENSE](../../LICENSE)）：可以自由使用、修改、再分发，包括商业闭源的产品，只要在分发时保留版权和许可声明。

### 第三方许可

SDK 的库里编进了下面这些第三方代码，**以二进制形式分发 SDK（也就是分发宿主程序）时要附上它们的许可声明**：

| 代码 | 用途 | 许可 |
| --- | --- | --- |
| libopus 1.6.1 | 默认的声音编码 | BSD 3-Clause（另有 Opus 的免版税专利许可） |
| NVIDIA NVENC 接口头文件（Video Codec SDK 13.0 和 12.0） | Windows 上直接调 NVENC（只有头文件，驱动在运行时加载；驱动 570 起用 13.0，522.25 起用 12.0） | MIT |
| Monocypher 4.0.3 | 短 PIN 的 PAKE（X25519、Elligator 2） | BSD 2-Clause（与 CC0 双许可，SDK 取 BSD） |
| BearSSL 0.6 | 没有系统密码学的系统（Android）上的密码学和 TLS | MIT |

全文在仓库根目录的 [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md)。`cmake --install` 把它和 `LICENSE` 装到 `share/RemoteControlSDK/`，AAR 和 XCFramework 的根目录里也各带一份；宿主把两者放进应用的“致谢”或许可页面。
