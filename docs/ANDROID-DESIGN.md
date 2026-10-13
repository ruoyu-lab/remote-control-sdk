# Android：平台说明

SDK 在 Android 上做什么、怎样做、为什么这样做。怎样在自己的应用里用（AAR、`rcs_android_initialize` 和 JNI、权限清单、显示画面、MediaProjection 和无障碍服务），见开发者指南的 [Android 快速开始](guide/06-quickstart-android.md)；接口以头文件为准（`include/rcsdk/rcsdk.h`、`rcsdk/android.h`、`rcsdk/gpu.h`）。SDK 的测试在 Samsung Galaxy S25+（Android 16，骁龙 8 Elite）上通过，验证到哪一步见 [ROADMAP](ROADMAP.md#当前状态)。

## 1. 要什么

- **观看端**：在安卓手机和平板上看、控 Mac 和 Windows：硬件解码（MediaCodec），GPU 帧（`AHardwareBuffer`），声音（AAudio），触屏手势、软键盘、手写笔，麦克风和回声消除，剪贴板，看的时候屏幕不熄。
- **分享端**：分享整个安卓屏幕（MediaProjection，系统每次都会问用户；Android 14 起用户也可以只选一个 App），设备放的声音（AudioPlaybackCapture）或麦克风。
- **被控**：安卓允许经**无障碍服务**（AccessibilityService）在整个系统里点按、滑动、返回、回主屏幕；这是宿主的组件（要声明、要用户在设置里打开），SDK 把对方的输入交给宿主（`RCS_INPUT_HOST`），示例实现这个服务。
- 不做：安卓上采集屏幕或注入输入的系统接口在 Java 里，SDK 是 C/C++ 库，不带 Java 代码；这些由宿主做（示例里有完整的做法），SDK 管编码、传输、加密和其余全部数据面。

## 2. 怎样做

1. **最低 Android 10（API 29）**，arm64-v8a 和 x86_64（模拟器、Chromebook；只编译过，见第 7 条）。要用的：`AHardwareBuffer_lockPlanes`（29）、`AImageReader_newWithUsage`、`AMediaCodec_createInputSurface`（26）、AAudio（27，输入预设 28）、`AMediaCodec_getName`（28）、AudioPlaybackCapture 和 mediaProjection 前台服务（29）。Qt 6.11 的工具链默认 `android-28`，用 SDK 时要写明 `ANDROID_PLATFORM=android-29`，更低时 CMake 配置就报错。
2. **密码学和 TLS：BearSSL 0.6**（`third_party/bearssl`，选项 `RCSDK_CRYPTO_BACKEND=bearssl`，Apple 系统和 Windows 以外的默认）：P-256 ECDH / ECDSA、AES、HMAC、SHA-256、PBKDF2、TURN 要的 SHA-1 和 MD5；TURN over TLS 用它的 TLS 1.2 客户端，信任系统的证书目录或证书指纹。arm64 上 AES-CTR 和 SHA-256 换成 SDK 自己写的 ARMv8 指令（`src/crypto/bearssl_armv8.cpp`，按 `getauxval(AT_HWCAP)`；没有时用 BearSSL 的软件）。
3. **新的公开接口**（`rcsdk/android.h`）：
   - `rcs_android_initialize(java_vm, context)` / `rcs_android_get_java`：宿主交出 JavaVM 和应用的 Context，SDK 和模块经它用 Java 的东西（麦克风权限、声音设备列表、剪贴板、唤醒锁）。没交出时这些以原因报告 `UNSUPPORTED`。
   - `rcs_session_push_hardware_buffer(session, buffer, {rotation})`：和 iOS 的 `rcs_session_push_sample_buffer` 对应，宿主把 MediaProjection（或相机、自己的渲染器）给的任意尺寸的 `AHardwareBuffer` 交过来，SDK 在 GPU 上转正、等比放进流的尺寸、画进自己的缓冲区，返回前画完，宿主随后就能把 Image 还给 ImageReader。实现时发现必须复制：BufferQueue 的生产者在宿主放手后就会重用这块缓冲区，不管别人还持有引用。
   - `RCS_GPU_AHARDWAREBUFFER`：观看端的 GPU 帧，以及 `rcs_session_push_gpu_frame` 收的类型（同样在推的时候复制）。
   - `RCS_PLATFORM_ANDROID`、`RCS_BACKEND_MEDIACODEC`。
4. **平台层 `src/platform/android/`**：
   - 编解码（`codecs.cpp`、`decoder.cpp`、`encoder.cpp`）：NDK 的 `AMediaCodec`。NDK 在 Android 15 之前没有编解码器列表，所以硬件的是 MediaCodec 为这个类型挑的那个（不是软件的时），软件的是 Android 自带、每台设备都有的 `c2.android.*` / `OMX.google.*`；能收多大的画面靠配置一次试出来（横竖都试，16:9 从 8192 往下）。模拟器的 `c2.goldfish.*` 把画面交给宿主电脑解码，全范围的颜色回来变成有限范围，不当作硬件。软件编码只有 H.264（和其他系统一样）。
   - 解码画进 `AImageReader`：GPU 帧用 `AIMAGE_FORMAT_PRIVATE`（解码器自己的排列，任何位深和色度），宿主要 CPU 帧时 4:2:0 8 位用 YUV_420_888，SDK 转成 BGRA8：CPU 能按三个平面映射的在 CPU 上转，映射不了的（高通的解码器即使给了 CPU 用途也输出压缩的 UBWC）和 10 位、4:4:4 的由 SDK 的 GL 线程当外部纹理画成 BGRA8 读出。帧持有 ImageReader 的图像，宿主最多同时持有 8 张。
   - 解码器收哪些画面格式：NDK 不说（档次只在 Java 的 `MediaCodecInfo` 里），所以和 Apple 的系统一样，把 Mac 硬件编码器做的 256×144 灰色关键帧（`src/platform/decode_probes.hpp`，H.264 4:4:4、HEVC 10 位 / 4:4:4 / 4:4:4 10 位）各解一次；解出画面、而且缓冲区的格式没有丢掉位深或色度（Android 的软件解码器在设备没有 10 位缓冲区时画成 8 位）才算收。AV1 的 Main 档次本来就含 10 位。S25+ 上：高通的 HEVC、AV1 解码器收 10 位，4:4:4 谁都不收（芯片不解）；软件的 HEVC 解码器不收 10 位，AV1 的收。探针最小 96×96（高通解码器的下限），所以做成 256×144。
   - 编解码器在 Android 16（Codec2 的 AIDL 接口）上每传一块缓冲区都回调进本进程（缓冲池、解码器的输出缓冲区）。App 进程本来就在接 binder 调用；原生程序（测试程序、经 adb 跑的工具）不接，编解码器就一直等。SDK 在第一次用 MediaCodec 前启动本进程的 binder 线程池（`libbinder_ndk` 的 `ABinderProcess_startThreadPool`，NDK 的头文件里没有，运行时取；App 里它什么也不做）。
   - 编解码器出错（例如收到它解不了的码流）时以 `RCS_ERROR_CODEC` 和原因结束，不当作“还没有画面”空等。
   - 编码用 MediaCodec 的输入 Surface：SDK 自己的一个 GL 线程（`gl.cpp`，OpenGL ES 2，无表面的上下文）把硬件缓冲区当外部纹理画上去（CPU 的 BGRA8 先上传），`eglPresentationTimeANDROID` 标时间，输出按时间对回帧。关键帧按请求（`request-sync`），码率运行中改（`video-bitrate`），不要 B 帧，SPS 声明不重排；量化参数的上限放开到 51（Android 12 起的 `video-qp-max`）：用编码器自己的上限时，带噪点的画面在码率调低后降不下去（Android 的 H.264 软件编码器要 0.6 Mbit/s 时仍出 1.4），节流就丢掉三分之二的帧。同一个 GL 线程也做推进来的硬件缓冲区的复制和放进流。
   - 编码只出 4:2:0 8 位：MediaProjection 给的是 8 位 RGB，屏幕分享用不上 10 位和 4:4:4 的编码。
   - 声音（`audio.cpp`、`aac.cpp`）：AAudio 播放（低延迟、媒体用途）；麦克风是 AAudio 的输入，`voice_processing` 时用 `VOICE_COMMUNICATION` 预设，否则 `VOICE_RECOGNITION`；设备没了自己重开，打不开时每秒再试、只报告一次原因。设备放的声音只能经 MediaProjection 在 Java 里录，由宿主推（`RCS_AUDIO_HOST`），`audio_capture_backends` 为 0。AAC 用 MediaCodec，Android 的编码器有比特池、没有真正的恒定码率，320 kbit/s 时包会超过一个数据报，所以只给 96～192 kbit/s 四档。
   - 采集来源只有 `HOST` 和 `ENCODED`；`rcs_capabilities.input_types` 为 0，输入交给宿主。
   - 前后台：安卓上进了后台也能解码和编码，SDK 不自己观察（`RCS_EXT_BACKGROUND` 不起作用），宿主按自己的生命周期调用 `rcs_session_pause`。
5. **网络**：Linux / 安卓的套接字（`MSG_NOSIGNAL`）；限定网卡用 `SO_BINDTODEVICE`（Linux 5.7 起普通进程可以），不行时明确报错。局域网广播要 Wi-Fi 的组播锁（宿主拿，示例拿了）。连上期间 SDK 拿低延迟的 Wi-Fi 锁（`WIFI_MODE_FULL_LOW_LATENCY`，经交出的 Java VM，要 `WAKE_LOCK`；`rcs_network_tuning_config.wifi_low_latency`）：Wi-Fi 省电时路由器把来的数据报攒到无线电醒来，每几秒一次 100 ms 以上，看的一端画面和声音一阵一阵地到；Android 只在应用在前台、亮屏时让这个锁生效。
6. **模块**：剪贴板经 `ClipboardManager`，带文字和 HTML（图片在 Android 上是内容 URI，模块不收发）；剪贴板的 `formats` 为 0 表示“本机剪贴板能带的全部格式”，这是默认值，所以默认设置在 Android 上也能直接用。保持唤醒经 `PowerManager` 的唤醒锁（`SYSTEM`：`PARTIAL_WAKE_LOCK`；`DISPLAY`：屏幕常亮）。文件传输在 `/data` 上改用 `renameat2(RENAME_NOREPLACE)`（Android 的 SELinux 不让建硬链接）。
7. **构建和测试**：预设 `android-arm64` 和 `android-x86_64`（从 macOS 交叉编译，NDK 的工具链 `cmake/toolchains/android.cmake`，`ANDROID_ABI` 选 ABI，API 29，静态库）；`scripts/build-android-aar.sh` 打出 `RemoteControlSDK-<版本>.aar`：每种 ABI 一个，核心、BearSSL、编进 SDK 的编解码库和全部官方模块合成一个去掉调试信息的静态库 `libRemoteControl.a`，加上头文件，是 Prefab 包（`RemoteControlSDK::RemoteControl`，导出要链接的系统库），清单要 `INTERNET` 和 `WAKE_LOCK`；静态库只要求宿主的 C++ 库也是 libc++（`c++_shared` 的宿主，例如 Qt，也能用）；`cmake/toolchains/android.cmake` 自己找 NDK，测试经 `cmake/android_run.sh` 去掉调试信息后推到模拟器或设备上运行（ctest 的 `CMAKE_CROSSCOMPILING_EMULATOR`），要先开着模拟器或接着设备；设备的 ABI 不对（例如 x86_64 的测试遇到 arm64 的手机）时测试以原因失败。要 Java 的部分（JNI、剪贴板、唤醒锁）在测试程序里没有 VM，跳过。
   - **x86_64**：给 x86_64 的模拟器和 Chromebook。平台层没有架构相关的代码（YUV 转换有 SSE 和 NEON 两种）；编解码库（libvpx、dav1d、libaom）的 x86-64 汇编要 NASM：NDK 的工具链为 x86 指定的是它自带的 YASM，dav1d 和 libaom 的汇编只收 NASM，所以 `android.cmake` 在 x86_64 上改用 `PATH` 里的 NASM（找不到时 CMake 自己找，找不到就报错；不想装就关掉 `RCSDK_WITH_*`）。Android 的 libc 有线程却没有 `pthread_cancel`，CMake 的 `FindThreads` 检查它失败后会给所有语言加 `-pthread`，NASM 不认，所以工具链直接说明 libc 有线程。验证：在 Apple 芯片的 Mac 上编译通过（库、模块、`rcsdk_cli`、测试，汇编都在、没有文本重定位），AAR 带两种 ABI；没有在 x86_64 的设备或模拟器上跑过（Apple 芯片跑不了 x86_64 的 Android 模拟器）。
8. **QML 示例的安卓版**：
   - 观看：和 iPad 一样的触屏手势、软键盘；画面用 OpenGL ES 的外部纹理直接画 `AHardwareBuffer`（`video_node_gles.cpp`，Qt Quick 在安卓上是 OpenGL ES）。
   - 分享：“允许远程连接”就开始：有声音时先问录音权限，再弹系统的共享屏幕对话框；前台服务里 MediaProjection 画进 `ImageReader`，每帧的 `HardwareBuffer` 推给 SDK，设备的声音经 AudioPlaybackCapture（排除示例自己）推给 SDK；转屏时虚拟显示器跟着换形状。
   - 被控：示例的无障碍服务把对方的点按、长按、拖动（一段一段接上的手势）、滚轮（滑动）、右键（返回）、中键（主屏幕）、按键和文字执行出来。
   - 局域网发现：拿 Wi-Fi 组播锁。

测试：`ctest --preset android-arm64` 经 adb 在模拟器或设备上运行（[BUILDING](BUILDING.md#android)）；x86_64 的是 `ctest --preset android-x86_64`，要 x86_64 的模拟器或设备。模拟器没有硬件编码器（`c2.goldfish` 解码器不算硬件），需要硬件的部分跳过；要 Java 的部分（JNI、剪贴板、唤醒锁）在测试程序里没有 Java VM，也跳过。
