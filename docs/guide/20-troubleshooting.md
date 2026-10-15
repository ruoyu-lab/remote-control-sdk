# 20 故障排查与常见问题

先打开日志：`rcs_set_log_callback(RCS_LOG_INFO, …)`，排查中的那个会话用 `RCS_EXT_LOG` 设成 `RCS_LOG_DEBUG`（[14](14-events-stats-logging.md#日志)）。失败时先看 `rcs_get_last_call_error`（调用被拒）和 `rcs_session_get_failure`（启动或运行失败）的原因文字，它们通常已经说明了缺什么。`rcsdk_cli --info` 列出本机的编解码实现、能力和权限。

## 连不上

| 现象 | 原因和办法 |
| --- | --- |
| 观看端一直 `CONNECTING`，几秒后 `NETWORK` | 地址、端口不对，或分享端的防火墙没放行（Windows 第一次监听时弹的确认框被关了；Windows 防火墙按程序路径放行）。分享端的 `connect.address` 要是本机的地址（或 `0.0.0.0`）。iOS 上还可能是没有本地网络授权（[05](05-quickstart-ios.md)）。第一次就连不上时观看端不重试 |
| `AUTHENTICATION`：“Wrong PIN or another secret” | 两端的密钥、口令或 PIN 不一样，或者一端用 `KEY`、另一端用 `PASSWORD`（原因会说明双方各用什么） |
| `AUTHENTICATION`：“The sharer accepts no more PIN attempts” | PIN 失败次数到了 `pin_attempts`；分享端的宿主要换一个 PIN（`rcs_session_set_identity`） |
| `AUTHENTICATION`：设备密钥相关 | 一端的设备公钥不在另一端的 `trusted_keys` 里；看 `rcs_peer_info.public_key` 是谁 |
| `UNSUPPORTED`：协议版本 | 两端不是同一个 SDK 版本（原因里写明两端的协议版本，0.1.1-beta 是 v1）。两端升级到同一个版本 |
| `UNSUPPORTED`：没有共同的编码格式 / 画面格式 / 声音格式 | 原因写明双方各提供了什么；多列一个两端都有的格式（默认的 H.264 几乎都有） |
| `BUSY` | 分享端满员（`max_viewers`），或解不了正在传的格式；重连中的观看端会自己重试 |
| `REFUSED` / `TIMEOUT`（准入） | 分享端的宿主拒绝了，或没在 `admission_timeout_ms` 内决定 |
| P2P 以 `NETWORK` 失败 | 两端的 NAT 打不通：原因写明检查了哪些地址、有几对得到回应。检查 STUN 服务器能不能访问；打不通就改走中继（[09](09-transports.md#直连失败)）。SDK 不会自动改走中继 |
| 中继 `AUTHENTICATION` | TURN 凭据错或过期，或 TLS 证书没通过（原因写明是哪一项：指纹、信任链、名字、有效期） |
| `HOST` 传输的观看端 5 秒后失败 | 分享端还没进入 `LISTENING`，或宿主的通道没把握手送过去；先启动分享端，或给观看端开着重连 |

## 启动就失败

| 现象 | 原因和办法 |
| --- | --- |
| `UNSUPPORTED`：没有硬件编码器 / 解码器 | 虚拟机、iOS 模拟器、Android 模拟器（模拟器的 `c2.goldfish` 不算硬件）：改用 `RCS_ACCELERATION_SOFTWARE` 或 `HARDWARE_FIRST`（两端都能用；VP8、VP9、AV1 由 SDK 自带的库编解，[18](18-errors.md#不悄悄降级时宿主怎样明确地退一步)） |
| `UNSUPPORTED`：Windows 上 HEVC / VP9 / AV1 的硬件解码 | 没装“HEVC 视频扩展” / “VP9 视频扩展” / “AV1 视频扩展”；把 H.264 也列上，或 VP9 / AV1 用 `SOFTWARE` / `HARDWARE_FIRST`（SDK 自带的库） |
| `UNSUPPORTED`：“this build has no libvpx (RCSDK_WITH_LIBVPX=OFF)” 之类 | 这份构建去掉了那个编解码库；换一个后端，或打开选项重新编（[BUILDING](../BUILDING.md)） |
| 构建时 “needs NASM on x86-64” | 装 NASM 并让 CMake 找到（`PATH` 或 `CMAKE_ASM_NASM_COMPILER`），或关掉 `RCSDK_WITH_*` |
| `UNSUPPORTED`：Windows N 版、Windows Server | 没有 Media Foundation（`mfplat.dll`）：装“媒体功能包” / “Media Foundation”功能；或者用 NVENC 和宿主自己的解码器 |
| `PERMISSION_REQUIRED` | macOS：屏幕录制、辅助功能、麦克风没授权；**重新签名后授权可能失效**（macOS 按代码签名记录），开发时固定签名（[03](03-quickstart-macos.md#系统权限)）。允许控制（`allowed = 1`）需要输入权限 |
| `INVALID_ARGUMENT`：版本 | 头文件和加载的库不是同一个版本（例如应用目录里留着旧的 DLL）：启动时比较 `rcs_version_number()` 和 `RCSDK_VERSION` |
| `INVALID_ARGUMENT`：结构体大小 | 没用 `*_INIT` 初始化；其他语言（Swift、C#）里手写的结构体大小不对（[02](02-threading.md#从其他语言调用)） |
| Android：剪贴板、保持唤醒、麦克风权限报告 `UNSUPPORTED` | 没调用 `rcs_android_initialize`（[06](06-quickstart-android.md)） |
| 分享端停在 `STARTING` | 挂了 `RCS_EXT_AGENT_LINK`，还没有助手连上来（[16](16-unattended.md)） |

## 画面

| 现象 | 原因和办法 |
| --- | --- |
| 画面停住，连接还在 | 采集暂停：看 `RCS_EVENT_CAPTURE` 的原因（锁屏、UAC、显示器睡眠 `DESKTOP`；窗口最小化 `HIDDEN`；窗口关了 `GONE`，宿主要换一个共享的东西）。或者暂停了：看 `RCS_EVENT_PAUSE`（`by_peer`：对方暂停了） |
| 分享端 Windows 上 `CAPTURE` 失败：“another part of this process duplicates the display” | 同一个进程里两次运行在采集同一个显示器；用一次运行加 `max_viewers` |
| 画质比预期差、帧率低 | 看 `RCS_EVENT_RATE` 和网络统计：路径排队或丢包时动态码率在降；满屏变化、码率不够时帧率会降（编码器的输出超过目标码率时 SDK 降帧率）。局域网上可以用 `LOW_LATENCY` 或 HEVC |
| 延迟高（尤其 120 FPS） | 观看端等 vsync：改成立即提交（[08](08-viewing.md#怎样画得快)）；帧先排到界面线程再画也会多一两帧。分享端的显示器刷新率也有影响：60 Hz 的屏比高刷屏慢 |
| 画面一阵一阵、码率被压低（Wi-Fi） | 接收端 Wi-Fi 省电：保持 `wifi_low_latency = 1`（Android、Windows）；看 `rcs_network_stats.wifi_low_latency` 是否保持着 |
| 偶尔卡一下、`RCS_EVENT_RECOVERY` | `BACKLOG`：解码或画面回调太慢，队列超时；`LOSS`：网络丢了补不回来的帧（看 `frames_lost`）；`DECODER`：解码器坏了换了一个 |
| 颜色不对 | 渲染器没按 `rcs_gpu_frame_info.color` 的矩阵和范围转换，或观看端列了渲染器不认识的画面格式（10 位、4:4:4） |
| Android 上新画面不来 | 宿主留着的 GPU 帧太多（最多 8 张）：画完就 `rcs_gpu_frame_release` |
| Windows 上同时分享几路时编码失败 | NVENC 的编码会话数有限（消费级显卡现在是 8 个） |

## 声音

| 现象 | 原因和办法 |
| --- | --- |
| 没有声音 | 看 `RCS_EVENT_AUDIO`（`direction`、`result`、`message`）：对方没开（“The viewer sends no sound” 一类的说明）、没有设备、没有麦克风权限、格式没有交集 |
| 回声 | 同一进程里既发本机的声音、又播放对方的声音：打开 `exclude_current_process`（[12](12-audio.md#回声和回环)）；观看端外放时麦克风开 `voice_processing` |
| 咔哒声、断音 | `underruns`（缓冲空了：网络抖动大）或 `device_underruns`（系统叫设备太晚：机器忙）；前者可以加大 `buffer_margin_us` 或 `max_delay_ms` |
| Wi-Fi 上画面一变化声音就断 | 网络统计的 `sound_queue_us`（收到的声音在路径上排了多久）在大画面（关键帧、整屏滚动）时跳高：声音排在接入点队列里的视频后面。看分享端的 `path_bps` 是否量出了路径速率（0：量不出，大画面照旧一次发出）；`path_burst_us` 调小让大画面摊得更开（[13](13-tuning.md#动态码率)）。`sound_wait_us` 高则是声音在分享端就等了（机器忙） |
| `start` 返回 `UNSUPPORTED`：回环 | 分享端发 `SYSTEM` 声音又由 SDK 播放观看端的声音，却没排除本应用（或助手在另一个进程）：按原因里的三个办法选一个 |

## 控制

| 现象 | 原因和办法 |
| --- | --- |
| `send_input` 返回 `INVALID_STATE` | 还没授权（等 `RCS_EVENT_CONTROL` 的 `granted`），或授权被暂停（被控端锁屏、UAC） |
| 点不动某些窗口（Windows） | 以管理员身份运行的窗口不接受普通进程注入的输入；看 `rcs_input_stats.last_result` |
| 按键卡住 | 观看端窗口失去焦点时没发 `RCS_INPUT_RELEASE_ALL` |
| Ctrl+Alt+Del 没反应 | 系统策略 `SoftwareSASGeneration` 没设（`PERMISSION_REQUIRED`），或进程不是服务 / 看护者启动的（`UNSUPPORTED`），见 [16](16-unattended.md) |
| 快捷键不对 | 键码按位置：Mac 的 Command 和 Windows 的 Ctrl 要宿主自己映射（[11](11-input.md#键码从平台的键码到-hid-用法码)） |

## 其他

- **回调里调 `stop` 返回 `CALLBACK_THREAD`、或者 C++ 程序退出时终止**：在会话自己的回调里停止或析构了它；派发到自己的线程（[02](02-threading.md)）。
- **第一次连接慢**：第一次列编解码实现要逐个打开编解码器（Mac 约 0.2 秒、PC 约 0.4 秒、手机约 0.6 秒）；启动时在后台线程里先调一次 `rcs_get_capabilities`。
- **macOS 的命令行宿主采集不到或卡住**：主线程的 run loop 要转着。
- **iOS 模拟器里分享端没有画面**：ReplayKit 在模拟器里能开始录制，但不出画面；只能在真机上试。
- 遇到看起来是 SDK 自己的问题：带上 `DEBUG` 日志、`rcsdk_cli --info` 的输出、两端的版本和系统，报告问题。
