# 19 各平台的差别

同一套接口在每个系统上都一样；下面是系统本身做得到的不同。做不到的都由 `rcs_capabilities` 报告、调用时以 `UNSUPPORTED` 和原因失败，不悄悄换成别的。系统自己的类型走各自的头文件（`rcsdk/apple.h`、`rcsdk/android.h`），在别的系统上是 `UNSUPPORTED`。

## 最低版本和构建

| | macOS | Windows | iOS / iPadOS | Android |
| --- | --- | --- | --- | --- |
| 最低系统 | macOS 14 | Windows 10 1803（单个窗口 1903，触摸和笔 1809） | iOS 17（强制硬件或软件编码 17.4） | Android 10（API 29），arm64-v8a、x86_64 |
| 打包 | 动态库（按次版本命名）或静态库，CMake 包 | DLL 或静态库（MSVC；也可在 macOS 上用 LLVM-MinGW 交叉编译），CMake 包 | XCFramework（静态），Swift `import RemoteControlSDK` | AAR（Prefab，两种 ABI），或预设 `android-arm64` / `android-x86_64` 的静态库 |
| 密码学 | 系统的 | 系统的（CNG、Schannel） | 系统的 | 编进 SDK 的 BearSSL |
| 快速开始 | [03](03-quickstart-macos.md) | [04](04-quickstart-windows.md) | [05](05-quickstart-ios.md) | [06](06-quickstart-android.md) |

其他系统（例如 Linux 桌面，SDK 不支持它，`rcs_capabilities.platform` 是 `RCS_PLATFORM_UNKNOWN`）只有可移植的部分：接口、会话、协议、传输、声音的编解码，以及 SDK 自带的视频编解码库（VP8、VP9、AV1，以及默认在这些系统上编进去的 OpenH264 的 H.264）。没有 SDK 自己的采集、系统的视频编解码和声音设备，所以分享端用宿主推的画面（`RCS_CAPTURE_HOST` 的 CPU 图像，自带的库编码）或码流（`RCS_CAPTURE_ENCODED`），观看端用自带的库解码成 CPU 帧（`on_frame`），或用宿主自己的编解码器（[17](17-replacing.md)）。Linux 的平台层不做（[ROADMAP](../ROADMAP.md)）。

## 能力对照

| | macOS | Windows | iOS / iPadOS | Android |
| --- | --- | --- | --- | --- |
| 观看：硬件解码 | H.264、HEVC、VP9（macOS 11 起）、AV1（M3 起） | H.264、HEVC、VP9、AV1（看显卡和系统的视频扩展；Intel 显卡上 oneVPL 不用扩展） | H.264、HEVC、VP9（iOS 26.2 起）、AV1（A17 Pro、M3 起） | H.264、HEVC、VP8、VP9、AV1（看芯片） |
| 观看：软件解码 | H.264、HEVC（系统的）；VP8、VP9、AV1（SDK 自带） | H.264、HEVC、VP8、VP9、AV1（系统的，后四种要视频扩展）；VP8、VP9、AV1（SDK 自带） | 同 macOS | H.264、HEVC、VP8、VP9、AV1（系统的）；VP8、VP9、AV1（SDK 自带） |
| 观看：10 位、4:4:4 | 有（按机器试出） | 有（HEVC 4:4:4 用 `HARDWARE_FIRST`：NVIDIA 显卡上 NVDEC、Intel 显卡上 oneVPL 在 GPU 上解，Intel 显卡上 VP9 4:4:4 也是，别的显卡上交给软件解码） | 有（按机器试出） | 有（按设备试出；S25+ 的 HEVC、AV1 解 10 位，高通的芯片不解 4:4:4） |
| 观看：GPU 帧 | `CVPixelBuffer` | D3D11 纹理 | `CVPixelBuffer` | `AHardwareBuffer` |
| 分享：画面 | SDK 采集显示器、区域、窗口、应用 | 同左 | 宿主推：应用自己的画面，或广播扩展里的整个屏幕 | 宿主推：MediaProjection 的整个屏幕或一个应用 |
| 分享：硬件编码 | H.264、HEVC（没有 VP9、AV1） | H.264、HEVC、AV1（看显卡；NVIDIA 默认 NVENC，Intel 默认 oneVPL，Intel 的 HEVC 4:4:4 也经 oneVPL），VP9（Intel 核显，经 Media Foundation） | H.264、HEVC | H.264、HEVC、VP8 / VP9（看芯片；只出 4:2:0 8 位） |
| 分享：软件编码 | H.264（系统的）；VP8、VP9、AV1（SDK 自带） | H.264（系统的）；VP8、VP9、AV1（SDK 自带） | 同 macOS | H.264（系统的）；VP8、VP9、AV1（SDK 自带） |
| 分享：系统交来的图像 | `CMSampleBuffer` | — | `CMSampleBuffer` | `AHardwareBuffer` |
| 分享：设备放的声音 | SDK 采集 | SDK 采集 | 宿主推（广播扩展的应用声音） | 宿主推（AudioPlaybackCapture） |
| 被控 | SDK 注入（要“辅助功能”） | SDK 注入 | 只在宿主自己的应用里（系统不许操作别的应用） | 整个系统，经宿主的无障碍服务 |
| 无人值守 | LaunchDaemon + LaunchAgent | 服务 + 看护者，跟随安全桌面 | — | — |
| 麦克风和回声消除 | 有 | 有（看声卡驱动） | 有 | 有（系统的语音通话预设） |
| 麦克风权限 | SDK 弹系统的提示 | 系统设置 | SDK 弹系统的提示 | 宿主的 Activity 申请，SDK 只报告状态 |
| 声音会话 | — | — | `rcs_set_audio_session_config` | —（系统没有这个概念） |
| 进后台 | — | — | SDK 自动暂停画面（系统不许后台用 GPU） | 不暂停（后台能编解码），宿主自己决定 |
| 剪贴板模块 | 文字、HTML、RTF、图片 | 同左 | 同左 | 文字、HTML |
| 剪贴板的文件（`FILES`，未在真机验证） | 读访达复制的文件；对端的文件只给访达一个隐藏占位，粘贴到哪里就取到哪里（进程要跑主运行循环，沙盒里不行） | 读 `CF_HDROP`；对端的文件是虚拟文件，资源管理器边读边写；以 LocalSystem 运行的助手两个方向都不行 | 读别的应用复制的文件（复制时拷到本机）；对端的文件是 `NSItemProvider`，被加载时取 | 读剪贴板里的 `content://`（复制时拷到本机）；粘贴对端的文件不支持 |
| 保持唤醒模块 | 系统、显示器 | 同左 | 只有显示器 | 系统、显示器 |
| 远程电源操作（电源模块） | 睡眠：root 或控制台用户；重启、关机、注销：控制台用户经 loginwindow，root 只能强制 | 有关机特权的进程全部能做（注销的是进程自己的 Windows 会话）；现代待机的机器不能睡眠 | 没有 | 没有 |
| 网络唤醒（电源模块） | 列出网卡（以太网、Wi-Fi，“唤醒以供网络访问”）；发唤醒包 | 列出网卡（设备管理器的唤醒设置）；发唤醒包 | 只发唤醒包（广播要多播授权） | 只发唤醒包 |
| 缩略图、文本插入点模块 | 有 | 有 | 没有（不能采集屏幕、读别的应用） | 没有（同左） |
| Wi-Fi 不省电（`wifi_low_latency`） | —（系统没有这个设置） | WLAN 媒体流模式、不做后台扫描 | — | 低延迟 Wi-Fi 锁（应用在前台、亮屏时生效） |
| 编码器每秒最多多少像素（`max_pixels_per_second`，分享端的帧率按它降） | —（VideoToolbox 不说；H.264 超过 Level 5.2 时改用流水线编码器，见 `rcs_codec_event`） | NVENC：驱动报告的最高 Level，4:4:4 按实际打得开的；Media Foundation、oneVPL 不说 | — | —（MediaCodec 的能力里有，还没读） |
| Wi-Fi 的信道和 AWDL（`rcs_list_wifi`、`wifi_facts`） | 全部：CoreWLAN 的频段、信道、带宽、信号、噪声、发送速率（不要定位权限），AWDL 是否开着 | 信道、信号、发送速率（WLAN API）；频段取自所连接入点的频率，没有定位权限的 Windows 11 可能不给，那时频段为 0 | 只有 AWDL 是否开着（系统没有读信道的接口） | 交出 Java VM 时：频段和信道（按频率）、信号、发送速率（`WifiManager.getConnectionInfo`，要 `ACCESS_WIFI_STATE`，AAR 自带） |
| 网络变化的通知 | Network.framework 的 `nw_path_monitor` | iphlpapi 的网卡、地址、路由通知 | 同 macOS | 每半秒看一次网卡和（交出 Java VM 时）`ConnectivityManager`；宿主的 `NetworkCallback` 可以接到 `rcs_notify_network_change` |
| 网络的种类、计费、省流量（`network_facts`） | 有：`nw_path` 的网卡类型、`expensive`、`constrained`（低数据模式）；VPN 按网卡名猜（`utun`、`ipsec`、`ppp`） | 有：网卡类型（`GetIfEntry2`）、网络列表管理器的费用（按流量计费、漫游、接近或超过流量上限）；VPN 按网卡类型（隧道、PPP、虚拟网卡；TAP 网卡算以太网） | 同 macOS | 交出 Java VM 时有（`ConnectivityManager`：传输、`NOT_METERED`、流量节省；要 `ACCESS_NETWORK_STATE`，AAR 自带）；没交出时只有网卡和默认网络（`excluded_networks` 是 `UNSUPPORTED`） |
| 套接字绑定到一块网络 | `IP_BOUND_IF` | `IP_UNICAST_IF` | 同 macOS | 交出 Java VM 时 `android_setsocknetwork`（按网络打标记），否则 `SO_BINDTODEVICE` |
| AAC 的码率 | 64～320 kbit/s | 96～192 kbit/s 四档 | 64～320 kbit/s | 96～192 kbit/s 四档 |
| Ctrl+Alt+Del | — | 条件见 [16](16-unattended.md) | — | — |

画面格式（4:4:4、10 位、HDR10）在每个编解码器上能做什么、做不到什么，见 [PICTURE-FORMATS](../PICTURE-FORMATS.md)。

## 系统权限

| | 分享屏幕和声音 | 远程输入（分享端，SDK 执行） | 麦克风 |
| --- | --- | --- | --- |
| macOS | “屏幕与系统音频录制”，屏幕和声音共用；`rcs_request_screen_capture_permission` 在主线程调用 | “辅助功能”；`rcs_request_input_permission` 在主线程调用 | “麦克风”；`rcs_request_microphone_permission` 在主线程调用；`Info.plist` 要有 `NSMicrophoneUsageDescription` |
| Windows | 采集的进程必须在已登录用户的交互式会话里（服务把采集交给用户会话里的助手） | 默认：输入桌面必须是解锁的普通桌面，控制不了以管理员身份运行的窗口；`secure_desktop` 打开且进程是 LocalSystem 时，锁屏、登录屏、UAC 和管理员窗口都能控制 | “设置 › 隐私 › 麦克风”里允许桌面应用访问；SDK 只读这个设置 |
| iOS | 不能采集屏幕和别的应用的声音（`UNAVAILABLE`）；应用自己的画面由宿主推（ReplayKit 开始前系统会问用户）；另有本地网络授权 | 不能注入（`UNAVAILABLE`）；输入交给宿主 | `rcs_request_microphone_permission` 第一次弹系统的提示（任何线程）；`Info.plist` 要有 `NSMicrophoneUsageDescription` |
| Android | SDK 不能采集（`UNAVAILABLE`）；宿主用 MediaProjection（系统每次都问用户；前台服务） | SDK 不能注入（`UNAVAILABLE`）；输入交给宿主的无障碍服务（用户在设置里打开） | `RECORD_AUDIO`，宿主的 Activity 申请；要先 `rcs_android_initialize` |

## 运行时查询

- `rcs_get_capabilities(&caps)`（`rcs_capabilities`，探测要几十毫秒到几百毫秒，结果在进程里缓存）：`platform`；画面：`capture_sources`、`picture_formats`、硬件和软件分别能编码 / 解码哪些格式（`1u << codec` 位）、`software_decoding_gpu_frames`、`pushed_gpu_frames`；声音：`audio_capture_backends`、`audio_exclusion`、`audio_encoders`、`audio_decoders`；控制：`input_types`、`secure_desktop`、`secure_attention`；网络：`transports`、`wifi_low_latency`、`network_facts`（本系统能说出网络的哪些事实：种类、计费、省流量；Android 在交出 Java VM 之后才有，所以每次调用时现查，不缓存）、`wifi_facts`（`rcs_list_wifi` 能说出 Wi-Fi 的哪些事实，同样现查）、`forwarder_links`（到转发器能走哪些链路：原生系统全部，浏览器只有宿主的通道 `HOST`）。
- `rcs_list_codec_implementations`：每一个编解码实现（[13](13-tuning.md#实现的选择)）。
- `rcs_list_displays`、`rcs_list_windows`、`rcs_list_audio_devices`、`rcs_list_networks`（本机的网络，默认网络在前）。
- 权限：`rcs_screen_capture_permission`、`rcs_input_permission`、`rcs_microphone_permission`（`RCS_PERMISSION_UNAVAILABLE` / `REQUIRED` / `GRANTED`）。
- 对端的：`rcs_peer_info`（平台、格式和最大画面、声音、接受的输入类型、设备公钥）。

`rcsdk_cli --info` 把本机的这些全部打印出来（[22](22-examples-tools.md)）。
