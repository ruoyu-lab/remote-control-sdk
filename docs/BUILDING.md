# 构建、测试与工具

## 环境要求

- 支持 C++20 的编译器，CMake 3.25 以上，Ninja（使用预设时）。
- x86-64 上（Windows、Intel 的 Mac、Android x86_64）要 [NASM](https://www.nasm.us/)（2.14 以上，CMake 的 `ASM_NASM` 要找得到：在 `PATH` 里或用 `-DCMAKE_ASM_NASM_COMPILER=...`）：SDK 自带的编解码库的汇编优化用它（libvpx、dav1d、libaom、OpenH264）。arm64 不用。不想装就关掉这些库的选项（见下面的 `RCSDK_WITH_*`）。
- macOS 14 以上（ScreenCaptureKit），Xcode 命令行工具。构建出的库最低要求 macOS 14.0（`CMAKE_OSX_DEPLOYMENT_TARGET`），与构建机的系统版本无关。
- Windows 10 1803 以上（桌面复制用 `DuplicateOutput1`，等待用高精度定时器，SDK 不再带旧系统的兼容路径）；只共享单个窗口（`RCS_CAPTURE_WINDOW`）要 Windows 10 1903 以上（Windows.Graphics.Capture），更早的系统在 `capture_sources` 里不报告它。静态库的使用者另需链接 `dwmapi`、`runtimeobject`、`version`、`secur32`、`crypt32`（macOS 上是 `Network` 框架）（用 CMake 的 `RemoteControl::Core` 时自动带上）。HEVC 解码需要安装 Microsoft 的“HEVC 视频扩展”（很多品牌机预装）；没有时能力里不报告 HEVC 解码，只列了 HEVC 的观看端会在 `start` 时返回 `UNSUPPORTED`（默认配置只用 H.264，不受影响）。在 macOS 上交叉编译 Windows 版需要 [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw)，默认放在 `build/toolchains/llvm-mingw-20260922-ucrt-macos-universal`，其他位置可用 `-DLLVM_MINGW_ROOT=...` 指定。
- iOS（观看端和分享端）：Xcode 和 iOS SDK，库最低要求 iOS 17.0（`CMAKE_OSX_DEPLOYMENT_TARGET`，iPadOS 同）。iOS 上默认编静态库（动态库在 iOS 上要做成 framework）；不编工具，示例只编 `rcsdk_cli`，QML 示例用 Qt 的 iOS 版另编（见下面的“iOS”）。
- Android：NDK（r27 测过；`cmake/toolchains/android.cmake` 依次找 `ANDROID_NDK`、`ANDROID_NDK_HOME`、`ANDROID_NDK_ROOT`，再找 `ANDROID_HOME`、`ANDROID_SDK_ROOT` 或 Android Studio 默认位置下最新的 `ndk/*`；`ANDROID_ABI` 是 `arm64-v8a`（默认）或 `x86_64`），库最低要求 Android 10（API 29），arm64-v8a 和 x86_64（模拟器、Chromebook；编解码库的汇编要 NASM，工具链用 `PATH` 里的 NASM，不用 NDK 自带的 YASM），默认静态库；不编工具。跑测试要 `adb` 能连到一个开着的模拟器或设备（`ANDROID_SERIAL` 选其中一个）。QML 示例另要 Qt 的 Android 版、JDK 17 起和 Android SDK（打 APK 时 Qt 的 Gradle 第一次会下载 Gradle 和 Android Gradle 插件）。
- QML 示例需要 Qt 6.6 以上（Quick、Quick Controls 2、Shader Tools），可选。
- Linux 上只有转发器（`server/forwarder/`，服务器；分享和观看、控制的两端不支持 Linux）：Ubuntu 24.04 LTS、26.04 LTS，x86-64 和 arm64，GCC 13 以上或 Clang 18 以上，`sudo apt install cmake ninja-build g++`，用预设 `linux-forwarder`（下面）。密码学用编进去的 BearSSL，不用系统的 OpenSSL。

## 预设

| 预设 | 内容 |
| --- | --- |
| `debug` / `release` | 库、示例、工具、测试 |
| `qt-release` | 再加上 QML 示例（`RCSDK_BUILD_QML_DEMO`） |
| `macos-x86_64` | Intel Mac 版（`CMAKE_OSX_ARCHITECTURES=x86_64`），在任何 Mac 上都能编；Apple 芯片的 Mac 上测试经 Rosetta 运行。libopus 按目标架构选 SSE 或 NEON，同时编两种架构（通用二进制）时用它的纯 C 实现 |
| `ios-simulator` | iOS 模拟器（arm64，静态库）：库、模块、`rcsdk_cli`、测试；测试在已经启动的模拟器里跑（`xcrun simctl spawn booted`），先 `xcrun simctl boot <设备>`。模拟器没有硬件编解码器（有 H.264 的软件编码器和 H.264、HEVC 的软件解码器），也不能采集屏幕：需要它们的测试和会话场景跳过（77），会话场景用软件编码和推送的画面照常跑 |
| `ios-device` | iOS 真机（arm64，静态库）：库和模块，不编示例和测试 |
| `android-arm64` | Android arm64-v8a（API 29，静态库），在 macOS 或 Linux 上用 NDK 交叉编译：库、模块、`rcsdk_cli`、测试；`ctest --preset android-arm64` 经 `cmake/android_run.sh` 把测试程序去掉调试信息推到 `/data/local/tmp/rcsdk` 上运行。模拟器没有硬件编码器、`c2.goldfish` 解码器不算硬件：需要硬件的部分跳过（77），会话场景用 Android 自带的软件编解码器照常跑，接着的设备上硬件编解码一起测；要 Java 的部分（剪贴板、唤醒锁、声音设备列表）在测试程序里没有 Java VM，跳过 |
| `android-x86_64` | Android x86_64（API 29，静态库）：同上，给 x86_64 的模拟器和 Chromebook，编解码库的汇编要 NASM；`ctest --preset android-x86_64` 要 x86_64 的模拟器或设备（设备的 ABI 不对时测试以原因失败）。Apple 芯片的 Mac 上只能编，跑不了 x86_64 的模拟器 |
| `asan` / `tsan` | Debug 加 AddressSanitizer + UndefinedBehaviorSanitizer / ThreadSanitizer（macOS、Linux；不编译工具），`ctest --preset asan` / `tsan` 带好了运行选项 |
| `windows-msvc` | 在 Windows 上用 MSVC 构建（从 Visual Studio 开发者命令行运行），动态库，`ctest --preset windows-msvc` |
| `windows-x64-release` / `windows-arm64-release` | 在 macOS 上用 LLVM-MinGW 交叉编译 Windows 版，静态库，产物只依赖系统 DLL |
| `linux-forwarder` | 服务器上的转发器（Linux 是它的首要平台）：BearSSL，不编示例、工具和编解码库；构建预设只编 `rcs_forwarder` 和它的测试，`ctest --preset linux-forwarder` 跑 `rcsdk.forwarder`、`websocket`、`udp`（那一跳的 UDP 连接）、`protocol`、`crypto`、`crypto_armv8`。部署只要 `--target rcs_forwarder`，见 [guide/24](guide/24-forwarder.md) |
| `linux-forwarder-scenarios` | 上一个再加 OpenH264（x86-64 上要 NASM）：还编并跑 `rcsdk.forwarder_tls`（`wss://` 经 Python 的 TLS 终结器）和经转发器的会话场景（`rcsdk.session.forwarder*`，两端是可移植的代码和推送的画面，只为在 Linux 上测转发器）。CI 的 Linux 作业用它 |

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

作为其他工程的子项目时，示例、工具和测试默认都不编译。相关选项：`BUILD_SHARED_LIBS`（默认 ON）、`RCSDK_MODULES`（构建哪些官方模块：`all`（默认，即 `clipboard;file_transfer;power;text_caret;thumbnail`），或从中选几个；每个是一个库：`RemoteControl::Clipboard`、`RemoteControl::FileTransfer`、`RemoteControl::Power`、`RemoteControl::TextCaret`、`RemoteControl::Thumbnail`，见 [MODULES](MODULES.md)）、`RCSDK_COMPONENTS`（会话之外的官方组件：`all`（默认）或 `supervisor`，库 `RemoteControl::Supervisor`）；`RemoteControl::All` 是核心加全部构建了的模块和组件、`RCSDK_BUILD_EXAMPLES`、`RCSDK_BUILD_TOOLS`、`RCSDK_BUILD_QML_DEMO`、`RCSDK_BUILD_FORWARDER`（转发器 `server/forwarder/`：共享库 `RemoteControl::Forwarder`（`rcsfwd.h`）和程序 `rcs_forwarder`，作为顶层工程时在 Linux、macOS、Windows 上默认 ON，iOS、Android、WebAssembly 上不编；由 SDK 的内部对象静态链进去，单独安装：`cmake --install <目录> --component forwarder`，带 `rcsfwd.h`、它用到的 `rcsdk/rcsdk.h`、`RemoteControlForwarderConfig.cmake` 和 `share/rcs-forwarder/` 下的配置示例、systemd 服务和它的 logrotate 规则、launchd 的 plist、许可证（`LICENSE`、`THIRD_PARTY_NOTICES.md`），部署见 [guide/24](guide/24-forwarder.md)）、`BUILD_TESTING`、`RCSDK_TRACE`（默认 OFF：Windows 发送端把每个阶段的时间戳打印到标准错误，只用于性能分析）、`RCSDK_CRYPTO_BACKEND`（密码学和 TURN over TLS 的 TLS 用谁的：`system`，Apple 系统和 Windows 的默认值；`bearssl`，编进 SDK 的 BearSSL（`third_party/bearssl`），其他系统（Android）的默认值。macOS 也能选 `bearssl`，用来测试它，例如 `cmake --preset release -B build/bearssl -DRCSDK_CRYPTO_BACKEND=bearssl`，再 `ctest --test-dir build/bearssl`；Windows 只有 `system`）、编进 SDK 的编解码库（每个都是静态库、符号隐藏并加 `rcsdk_` 前缀，源码在 `third_party/`，构建时不下载）：`RCSDK_WITH_LIBVPX`（VP8、VP9 编解码）、`RCSDK_WITH_DAV1D`（AV1 解码）、`RCSDK_WITH_LIBAOM`（AV1 编码），默认 ON（WebAssembly 里 OFF）；`RCSDK_WITH_OPENH264`（H.264 编解码），默认只在没有平台层的系统上 ON（支持的系统都有自己的软件 H.264，要用也能打开）。关掉的库不出现在实现列表里，选它的后端以原因 `UNSUPPORTED`；四个库在 macOS arm64 的发布版里约占 7 MB。

## 测试

`ctest --preset <预设>` 运行全部测试。每个测试程序是一个 ctest（`rcsdk.<名字>`），模块和组件的是 `rcsdk.<模块>.<名字>`；本机做不到的（没有 H.264 编解码器、不能采集屏幕、没有 Python 和 openssl……）返回 77，ctest 记为跳过。用固定端口的测试共用一个资源锁，`ctest -j` 不会互相占端口。每个程序覆盖什么写在它的源文件开头的注释里。

`session` 是回环上的完整分享和观看，按场景拆成独立的 ctest（`rcsdk.session.<场景>`，每个 90 秒上限，传 1 GiB 的 `file_transfer_large` 180 秒）：`rcsdk_test_session --list` 列出全部场景，`rcsdk_test_session <场景>` 单独运行一个，不带参数时依次运行全部。它通过只供测试使用的接缝（`src/seams`）用测试图案代替屏幕采集、用正弦波代替系统声音，所以不需要录屏权限，也不会发出声音。

| 测试程序 | 覆盖什么 |
| --- | --- |
| `c_api`、`public_headers`、`view_geometry` | C 接口按 C 编译和链接、版本、默认值、结构体大小、扩展链和参数的校验、预设；公开头文件在 `-Wall -Wextra -Wpedantic -Werror` 下按 C 和 C++ 编译，每个 `*_INIT` 初始化所有字段；`view_geometry.hpp` 的换算 |
| `session` | 回环上的完整会话，按场景（见上） |
| `agent` | 会话和助手之间的链接：消息、本机端点、助手的接上、替换和离开 |
| `protocol`、`queues`、`negotiation`、`signals` | 消息的往返和畸形消息的拒绝；接收积压和输入队列；编解码协商和画质规划；P2P / 中继信令的路由 |
| `secure_channel`、`udp`、`tcp`、`fec`、`host_channel` | 握手和加密记录（助手链接的通道）；UDP 传输（重传、节流、cookie、关闭通知）；TCP 传输（分享端的端口并行握手、换一条 TCP 接着走、伪造和重放的数据报、洪水）；Reed-Solomon 冗余；宿主的通道当作数据报套接字 |
| `stun`、`ice`、`turn`、`tls`、`tls_relay` | STUN 消息；ICE 收集和连通；对本机最小 TURN 服务器的中继（UDP、TCP）；TURN 证书的校验；经 TLS 终结器的 TURN over TLS（要 Python 和 openssl） |
| `crypto` | 构建所用的密码学（系统的或 BearSSL）：标准向量、ECDH、ECDSA、认证加密 |
| `crypto_armv8` | BearSSL 构建：ARM64 上 AES-CTR、SHA-256 的 ARMv8 指令（`src/crypto/bearssl_armv8.cpp`）对标准向量和 BearSSL 的软件（随机的密钥、长度、对齐、回绕的计数器、拆开的输入）；处理器没有这些指令时跳过。`rcsdk_test_crypto_armv8 --bench` 量两者每秒的 MB |
| `forwarder`、`forwarder_tls`、`websocket` | 转发器（编了它时）：令牌、流密钥、那一跳的消息，环回上的转发器对违规的端；`wss://` 经 TLS 终结器到它的 WebSocket 入口（要 Python、openssl 和 H.264）；WebSocket 的开头和帧、三种分帧 |
| `web_json`、`web_decoded`、`web_serve` | 浏览器包的 JSON；浏览器解码器的画面配对和尺寸检查；示例服务器 `examples/web/serve.mjs` 默认安全（node，没有 node 时不加这项） |
| `rate_control` | 动态码率在模拟瓶颈上的决策、规则、宿主的决定 |
| `audio` | Opus、AAC、PCM 的往返，丢包补偿，播放缓冲在抖动和时钟漂移下的表现 |
| `mixer` | 宿主的两种 sample buffer 声音（应用的、麦克风）混成一路：不同的块长和节奏、一种停了又来、成批到达、时钟漂移和丢块下延迟不变长、音量和不要某一种；Apple 系统上还有合成的 `CMSampleBuffer`（ReplayKit 的格式、不交错的 float32、16 kHz）的转换和会话的宿主声音 |
| `h264`、`av1`、`nal`、`sampling`、`pictures`、`yuv` | 码流工具：SPS 改写、AV1 OBU、NAL 单元；参数集里的色度和位深；画面尺寸和编码格式的上限；YCbCr 到 BGRA（SIMD 和标量一致） |
| `codec_roundtrip` | 本机每种“编码格式 × 硬件 / 软件”组合编码再解码；不支持的组合报错而不是被替换 |
| `cursor`、`cursor_stream`、`keys` | 指针图像的缩小；单独传送的指针的编号；HID 用法到每个系统的按键 |
| `modules`、`thumbnail` | 模块的机制（调用不重叠、队列、机器那一半）；`rcs_capture_picture` 和缩略图模块 |
| `capture_targets`（macOS、Windows） | 真实桌面上用测试自己的窗口采集窗口、应用、区域（不能采集屏幕时跳过） |
| `surface_turns`（Windows） | 旋转的显示器的画面在 GPU 上转正 |
| `ios_platform`（iOS 模拟器） | 声音会话和应用的音频引擎、前后台通知、iOS 版的剪贴板和保持唤醒模块 |
| `clipboard.content` | 剪贴板内容的线上形式和监视；`RCSDK_TEST_CLIPBOARD=1` 时也读写系统剪贴板 |
| `file_transfer.paths`、`file_transfer.messages`、`file_transfer.store` | 文件传输的路径规则、消息、机器那一半的读写（符号链接、临时文件、续传记录） |
| `text_caret.reader` | 插入点的坐标换算；Windows 上 `RCSDK_TEST_INPUT=1` 时读真实的系统插入点 |
| `supervisor.api`、`supervisor.os_sessions`、`supervisor.run`（Windows） | 看护者组件的 C 接口和参数校验、会话列表；以 LocalSystem 运行时真正启动和看护进程（否则跳过） |
| `installed_package` | 安装 SDK 后，由独立工程用 `find_package` 构建并运行 |

在 Windows 上，可以用 `windows-msvc` 预设在本机构建并运行 `ctest`，也可以把交叉编译的 `build/windows-x64-release/bin/rcsdk_test_*.exe` 拷到目标机器上逐个运行。

### 模糊测试

`tests/fuzz/` 是 libFuzzer 的有状态模糊目标，每个按输入写出的一串步骤驱动 SDK 的一块（文件开头写着步骤）：`udp_video`（观看端的 UDP 连接从分片和冗余拼画面、可靠消息、时间流逝、会话跟不上）、`secure_channel`（加密记录流和其中的消息）、`file_transfer`（文件传输会话那一半的状态机：新的、重发的、续传的报价，回答和纪元，存储的回答，宿主的接受和取消，对端和机器那一半的离开和回来）、`forwarder_entry`（转发器的入口一次次读一条连接的字节：TCP 入口的长度、WebSocket 入口的开头和帧；端那一边：服务器对开头的回答和它的帧、TCP 的长度；只在编了转发器时有）。它们不是 ctest，只在 `RCSDK_BUILD_FUZZERS=ON` 的构建里编译：整个 SDK 带 AddressSanitizer、UndefinedBehaviorSanitizer 和 libFuzzer 的覆盖率编译，并定义 `RCSDK_FUZZING`（打开 `UdpFuzzEnd` 这样的入口）。要带 libFuzzer 的 clang：macOS 上用 Homebrew 的 LLVM（Apple 的 clang 没有 libFuzzer）：

```bash
cmake -S . -B build/fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DCMAKE_OBJCXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DCMAKE_OSX_SYSROOT=$(xcrun --show-sdk-path) \
  -DRCSDK_BUILD_FUZZERS=ON -DRCSDK_BUILD_EXAMPLES=OFF -DRCSDK_BUILD_TOOLS=OFF \
  -DRCSDK_WITH_LIBVPX=OFF -DRCSDK_WITH_DAV1D=OFF -DRCSDK_WITH_LIBAOM=OFF
cmake --build build/fuzz --target rcsdk_fuzz_udp_video rcsdk_fuzz_secure_channel rcsdk_fuzz_file_transfer rcsdk_fuzz_forwarder_entry
mkdir -p corpus/udp_video && build/fuzz/bin/rcsdk_fuzz_udp_video -max_total_time=300 -rss_limit_mb=4096 corpus/udp_video
```

崩溃样本写在当前目录（`-artifact_prefix=` 可以改），`build/fuzz/bin/rcsdk_fuzz_udp_video crash-…` 重放一个。`udp_video` 每次运行开一个绑定 `127.0.0.1` 随机端口的套接字（不收不发），所以不占固定端口。

## 示例和工具

`examples/` 和 `tools/` 里的程序只用来测试和演示 SDK，不是 SDK 的一部分，也不代表任何产品。每个程序的用法见 [examples/README.md](../examples/README.md) 和 [tools/README.md](../tools/README.md)；`rcsdk_cli` 的全部选项见 `rcsdk_cli --help`。

```sh
./build/release/bin/rcsdk_cli --generate-key key.bin
./build/release/bin/rcsdk_cli --role share --address 192.168.1.10 --key-file key.bin --allow-control
./build/release/bin/rcsdk_cli --role view  --address 192.168.1.10 --key-file key.bin --seconds 30 --report view.json
```

Windows 防火墙按程序路径放行，第一次让某个程序监听端口时会弹出确认框。

## iOS

**XCFramework**：`scripts/build-xcframework.sh [输出目录]`（默认 `build/xcframework`）编真机（arm64）和模拟器（arm64 + x86_64）的静态库，把核心、编进 SDK 的编解码库和官方模块合成一个 `libRemoteControl.a`，带上 C 头文件和 `module.modulemap`，做成 `RemoteControl.xcframework`。Swift 里 `import RemoteControlSDK`（`*_INIT` 宏 Swift 导入不了，见[指南 02](guide/02-threading.md#swiftiosmacos)）；要链接的系统框架见[指南 05](guide/05-quickstart-ios.md#引入-sdk)。

**QML 示例**（要 Qt 的 iOS 版，Qt 6.6 起；下面用 Qt 6.11 的路径）：

```sh
# iPad / iPhone 真机：生成 Xcode 工程（TEAM 是 Xcode 设置里账户的团队 ID），在 Xcode 里运行
~/Qt/6.11.2/ios/bin/qt-cmake -S . -B build/qml-ios -G Xcode -DCMAKE_OSX_SYSROOT=iphoneos \
    -DBUILD_TESTING=OFF -DRCSDK_BUILD_TOOLS=OFF -DRCSDK_BUILD_QML_DEMO=ON -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=TEAM -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE=Automatic
open build/qml-ios/RemoteControlSDK.xcodeproj
# 模拟器：Qt 6.11 的 iOS 版只带 x86_64 的模拟器库，在 Apple 芯片的 Mac 上经 Rosetta 跑
~/Qt/6.11.2/ios/bin/qt-cmake -S . -B build/qml-ios-sim -G Xcode -DCMAKE_OSX_SYSROOT=iphonesimulator \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 -DBUILD_TESTING=OFF -DRCSDK_BUILD_TOOLS=OFF -DRCSDK_BUILD_QML_DEMO=ON \
    -DBUILD_SHARED_LIBS=OFF
cmake --build build/qml-ios-sim --target rcsdk_qml_demo --config Release -- CODE_SIGNING_ALLOWED=NO
xcrun simctl install booted build/qml-ios-sim/bin/Release/RemoteControlQmlDemo.app
xcrun simctl launch booted local.rcsdk.qmldemo --connect <IP> --code 123456
```

模拟器没有硬件编解码器：示例在没有硬件编码器的机器上用软件编码、观看时硬件优先；模拟器里 ReplayKit 能开始录制但不出画面，广播也开不起来，分享 App 画面和整个屏幕只能在真机上试。App 和它的广播扩展（`rcsdk_qml_demo_broadcast`，嵌在 App 的 `PlugIns/` 里）共用一个钥匙串访问组（`examples/qml-demo/shared.entitlements`），免费的个人团队也能签。真机第一次运行前：iPad 上打开“设置 › 隐私与安全性 › 开发者模式”；装上后在“设置 › 通用 › VPN 与设备管理”里信任自己的开发者证书。

## Android

**库**：`cmake --preset android-arm64 && cmake --build --preset android-arm64` 得到 `build/android-arm64/lib/` 下的静态库（核心、编解码库、官方模块和组件）；x86_64 用预设 `android-x86_64`（要 NASM）；把 SDK 当子项目编进宿主时用宿主的 NDK 工具链，`ANDROID_PLATFORM` 至少 `android-29`。要链接的系统库和要交出的 Java VM 见[指南 06](guide/06-quickstart-android.md)。

**AAR**（给用 Gradle 的宿主）：`scripts/build-android-aar.sh [输出目录 [ABI...]]`（默认 `build/aar`、`arm64-v8a x86_64`，要 `zip`，x86_64 要 NASM）得到 `RemoteControlSDK-<版本>.aar`：每种 ABI 把核心、BearSSL、编进 SDK 的编解码库和全部官方模块合成一个去掉调试信息的静态库，加头文件，是 Prefab 包（`find_package(RemoteControlSDK REQUIRED CONFIG)`、`RemoteControlSDK::RemoteControl`）。用法见[指南 06](guide/06-quickstart-android.md#引入-sdk)。

**测试**：先开一个模拟器（例如 `emulator -avd <名字> -no-window -gpu swiftshader_indirect`，数据分区留几百 MB）或接上开了 USB 调试的设备，再 `ctest --preset android-arm64`；x86_64 的在 x86-64 的电脑上开 x86_64 的模拟器（或接 x86_64 的设备、开了 adb 的 Chromebook）后 `ctest --preset android-x86_64`。

**QML 示例**（要 Qt 的 Android 版，下面用 Qt 6.11 的路径；Qt 的工具链默认 API 28，要写明 29）：

```sh
export JAVA_HOME=/path/to/jdk-17-or-later
~/Qt/6.11.2/android_arm64_v8a/bin/qt-cmake -S . -B build/qml-android -G Ninja \
    -DQT_HOST_PATH=$HOME/Qt/6.11.2/macos -DANDROID_SDK_ROOT=$HOME/Library/Android/sdk \
    -DANDROID_NDK_ROOT=$HOME/Library/Android/sdk/ndk/27.2.12479018 -DANDROID_PLATFORM=android-29 \
    -DCMAKE_BUILD_TYPE=Release -DRCSDK_BUILD_QML_DEMO=ON -DBUILD_TESTING=OFF
cmake --build build/qml-android --target apk
# 未签名的 APK 用调试密钥签上再装（或在 Android Studio 里打开 build/qml-android/examples/android-build）
apksigner sign --ks ~/.android/debug.keystore --ks-pass pass:android --key-pass pass:android \
    --out RemoteControlQmlDemo.apk build/qml-android/examples/android-build/build/outputs/apk/release/android-build-release-unsigned.apk
adb install -r RemoteControlQmlDemo.apk
```

示例的 Java 部分（共享屏幕的前台服务、无障碍服务）和清单在 `examples/qml-demo/android/`，由 Qt 一起打包。被控要在设备的“设置 › 无障碍”里打开“远程控制 Demo：对方的操作”；Samsung 的系统里在“设置 › 辅助功能 › 已安装的应用”。

## 测量工具

`tools/` 下的程序用于性能测量和维护，不随 SDK 发布，用法见 [tools/README.md](../tools/README.md)，数据和解读见 [PERFORMANCE.md](PERFORMANCE.md)。Android 上的探针要另配一个带工具的目录：`cmake --preset android-arm64 -B build/android-tools -DRCSDK_BUILD_TOOLS=ON -DBUILD_TESTING=OFF`，再 `adb push` 到 `/data/local/tmp`。用 CMake 选项 `-DRCSDK_TRACE=ON` 构建时，Windows 发送端会在标准错误输出每个阶段的时间戳（默认不编译进去）。
