# 架构

## 定位

本项目是**通用的端侧远程桌面与屏幕共享 SDK**。它不是框架，也不是为某一个产品定制的平台 SDK：宿主应用调用它，宿主掌控自己的业务流程。

- **适用的宿主**：宿主是开发者基于 SDK 编写的软件。SDK 本身不是给最终用户直接使用的程序。常见的宿主有：
  - 独立的远程桌面和远程协助软件；
  - 把看屏、控屏嵌进自己的应用，例如协作、教育、客服、即时通讯；
  - 设备与机房管理、云桌面；
  - 只在局域网里工作的工具。

  这些宿主用同一套接口，没有哪一类是默认对象。
- **两层用法**：
  - **开箱即用**（对接入的开发者而言）：只填核心配置（角色、预设、怎么连、怎么证明身份）就能跑通，默认值（预设 `BALANCED`）按一般情况在画质、延迟、带宽和资源占用之间均衡取舍；另有几个场景预设。
  - **深度定制**：每一项行为都能通过功能扩展、调优扩展调整，内部的一环也可以由宿主接管（替换层）；内部策略的每次变化都能从事件和统计里看到。
- **数据面归 SDK**：屏幕和声音采集、编解码、加密传输、积压恢复、输入执行和权限检查。这些都由 SDK 统一实现，保证所有宿主得到一致的性能和行为。
- **控制面归宿主**：用户与设备身份、配对或邀请、授权与审批、信令通道、界面。宿主可以有自己的服务器，也可以完全没有服务器（例如局域网工具）。SDK 不依赖任何服务器，也不限制后端使用的语言。
- **不为某一类宿主做假设**：
  - 默认值、命名、示例和文档都不偏向某种产品。
  - 某个场景需要、SDK 还没有的能力（例如无人值守），作为通用能力列进 [ROADMAP](ROADMAP.md#通用化还缺的能力)，而不是为某个宿主单独绕过去。
- **能力由宿主决定，不悄悄替换**：硬件还是软件编解码、提供哪些编码格式、要 GPU 帧还是 CPU 帧，都由宿主配置。做不到时返回明确的错误码和原因（`start` 的返回值，或运行失败后的 `rcs_session_get_failure`），由宿主决定下一步。SDK 内部只保留不改变能力的策略，例如丢包后用关键帧恢复、采集节拍、macOS 实时编码器与流水线编码器之间的切换（都是硬件编码，切换结果在统计里可见）。
- **框架中立**：核心只依赖 C++ 标准库、系统 API 和随 SDK 一起编译的几个库（libopus；短 PIN 用的 Monocypher；系统没有密码学时的 BearSSL；视频编解码库 libvpx、dav1d、libaom、OpenH264，见“依赖原则”），不提供 Qt 或 Node.js 专用的包。`examples/` 和 `tools/` 里的程序（命令行工具、Windows 原生观看端、QML 示例、测量工具）只用来测试和演示 SDK，不是 SDK 的一部分，也不代表任何产品。
- **接口**：C ABI（`rcsdk.h`）便于其他语言绑定，另附一个纯头文件的 C++ 包装（`rcsdk.hpp`）。配置分为核心（角色、预设、连接、身份）和可选的扩展链，按入门、功能、调优与替换三层组织（见“接口的三层与预设”）；变化通过一个类型化的事件回调汇报；统计按分区提供。新能力以新的扩展、事件、统计分区和函数加入。1.0 之前不保留兼容：每个版本都可以调整结构体、枚举和协议，头文件与库必须是同一版本，两端运行同一版本（规则见 [API-DESIGN.md](API-DESIGN.md#3-版本规则10-之前)，每个版本的改动见 [CHANGELOG.md](../CHANGELOG.md)）。

## 分层

```text
include/rcsdk/        公开接口：rcsdk.h（C ABI，入门和功能层）、tuning.h（调优）、host_codec.h、rate_control.h、host_transport.h、gpu.h、
                      agent.h（替换层）、module.h（模块）、apple.h 和 android.h（系统自己的类型），各有 C++ 包装 .hpp（rcsdk.hpp
                      是入门和功能层）；view_geometry.hpp
src/api/              C ABI：核对结构体大小与本版本一致、异常转结果码和原因（rcs_get_last_call_error）
src/session/          会话生命周期、事件、分区统计；config 把核心配置和扩展链解析并校验成内部配置，并给出预设的值（presetSettings、
                      applyPreset），checks 核对助手的机器能否做到（启动时全部，
                      运行中修改时只查改了的扩展，助手在另一个进程时在它连上时再查）；share.cpp / view.cpp 两个角色（分享端拆成
                      share_viewer：观看端消息与控制授权，share_video：流公告与发送，share_agent：本次运行的助手）；stream_plan：
                      画质由谁决定；cursor_stream：单独传送的指针；host_sources：宿主送入的画面和声音、交给宿主的输入；
                      编解码协商、接收队列、输入队列、帧交付
src/agent/            助手：分享端和这台机器有关的一半。agent.hpp 是会话和助手之间的接口（Port：会话的要求；Events：助手的报告）；
                      media_agent：在本机实现它（video_pipeline：采集、编码、码率预算的循环；声音采集；输入注入；显示器和输入可用性）；
                      remote：会话一侧的另一个进程里的助手；agent_server：助手一侧（rcs_agent）；link_messages：链接的前缀和消息
src/protocol/         消息格式与校验（messages）、连接接口（connection）、数据报连接（udp_connection，所有传输上都是它：可靠消息、视频分片、选择性重传、发送节流、路径和迁移）、按连接编号分路的 UDP 端口（udp_demux）和 TCP 端口（tcp_demux）、动态码率（rate_control）、路径的短断流（stall_watch）
src/transport/        套接字与 IPv4 / IPv6 地址（socket：UDP 套接字和"数据报套接字"接口）、网卡列表（interfaces）、网络变化的代数（network_changes）、字节流接口（stream）及其实现：可取消的 TCP 字节流（tcp_stream）、本机端点（local_stream：Windows 命名管道 / Unix 套接字）；握手（handshake）、认证加密记录通道（secure_channel，跑在任何字节流上）、STUN 消息（stun）、ICE（ice，含运行中的重启信令）、TURN 客户端（turn，含 RFC 8016 的移动）及其 TCP 承载（turn_tcp_socket）、字节流上的数据报（framed_socket：TURN 的 TCP、到转发器的 TCP 和 WebSocket）、WebSocket（websocket：RFC 6455 的开头和帧）、宿主自己的通道（host_channel）
src/crypto/           密码学：系统的 Security / CommonCrypto / CNG（crypto），或系统没有时的 BearSSL（crypto_bearssl：Android 和没有平台层的系统，`RCSDK_CRYPTO_BACKEND`；
                      ARM64 上它的 AES-CTR 和 SHA-256 换成处理器的 ARMv8 指令：bearssl_armv8）；
                      ECDH、AES、HMAC，以及两者共用、在其上组合的 HKDF 和认证加密（composed）
src/audio/            声音编解码（Opus 与 PCM 跨平台，AAC 取自平台）、观看端播放缓冲（抖动、时钟漂移）
src/media/            与编解码器无关的帧和数据包类型、NAL 单元工具（H.264 / HEVC）、H.264 SPS 改写、AV1 的 OBU 和序列头、VP8 / VP9 的帧头和
                      `vpcC`（vpx）、YCbCr 的 CPU 画面（yuv_image：NV12 / P010 / NV24 / P410 和平面格式之间、BGRA 之间的转换）、编码器输出的拆分
src/codecs/           随 SDK 编译的编解码库（libvpx、libaom、dav1d、OpenH264，每个系统相同）：实现列表合并系统的和库的（factory），
                      库的编码器和解码器共有的部分（software_codec），每个库一个文件；平台只提供采集画面 → YuvImage、YuvImage → 本系统的帧
src/platform/         平台接口 platform.hpp；apple/（macOS 和 iOS 共用的 Objective-C++：VideoToolbox 编解码和编解码器列表、像素缓冲和帧访问、
                      宿主推来的 CVPixelBuffer、AAC、Network.framework 的 TLS）、macos/（采集、声音采集和播放、输入、指针、窗口、桌面状态）、
                      ios/（能力、应用的音频引擎和声音会话：播放和麦克风、应用的前后台）、android/（MediaCodec 编解码和编解码器列表、
                      AImageReader 的硬件缓冲区帧、SDK 自己的 GL 线程：编码器的输入 Surface 和推来的缓冲区的复制、AAudio 的播放和麦克风、
                      MediaCodec 的 AAC、宿主交出的 Java VM）、windows/（win：句柄、UTF-8 / UTF-16、错误文字、令牌查询；desktop：输入桌面、
                      桌面状态、Ctrl+Alt+Del；helper：被看护进程一侧）、other/（desktop：没有安全桌面和看护者的平台，Ctrl+Alt+Del 和被看护
                      进程一侧 UNSUPPORTED；application：没有前后台和声音会话的平台；java：没有 Java 的平台）、unsupported/
src/modules/          模块的机制（rcsdk/module.h 的实现）：进程的注册表（registry）、模块的一半和它不重叠的调用（module）、
                      机器那一半在哪里跑（machine_host：分享端的助手里，或观看端自己的进程里）
src/seams/            创建机器相关对象的地方（屏幕采集、输入注入和输入权限、声音采集、桌面状态、Ctrl+Alt+Del、应用的前后台）：生产代码用平台的，SDK 自身的测试可以替换
src/util/             时钟、错误类型、日志、大端整数和十六进制
third_party/opus/     libopus 1.6.1（不含神经网络部分），作为隐藏符号的目标文件编进 SDK
third_party/monocypher/  Monocypher 4.0.3（短 PIN 的 PAKE 用它的 X25519、Elligator 2、SHA-512），符号改名隐藏
third_party/bearssl/  BearSSL 0.6：系统没有密码学和 TLS 时（Android，`RCSDK_CRYPTO_BACKEND=bearssl`）
third_party/nvenc/    NVENC 的接口声明 13.0 和 12.0（只有头文件，驱动在运行时加载）
third_party/nvdec/    NVDEC（CUVID）和它用到的 CUDA 驱动接口的声明（只有头文件，驱动在运行时加载）
third_party/onevpl/   oneVPL 的接口声明（只有头文件；Intel 显卡驱动带的运行时在运行时加载，不带调度器）
third_party/libvpx/、dav1d/、libaom/、openh264/  视频编解码库（`RCSDK_WITH_*`），各是静态库、符号改名隐藏；libvpx 和 libaom 按架构
                      预先生成的配置（generate-config.sh），dav1d 和 OpenH264 的 CMake 照它们自己的 meson 写；rename-symbols.sh 生成改名表
modules/              官方模块，各是一个库，只用公开头文件：clipboard（剪贴板，含四个系统的剪贴板读写）、file_transfer（文件传输，含
                      POSIX / Windows 的文件读写）、power（保持唤醒，含 Windows / macOS / iOS / Android 的电源请求）、thumbnail（在会话的
                      助手那里取图）、text_caret（文本插入点，含 Windows / macOS 的读取器）
components/           会话之外的官方组件，各是一个库，只用公开头文件：supervisor（Windows 的看护者和会话列表）
```

各层只向下依赖：`api → session`；`session → agent、protocol、audio、platform`；`agent → protocol`（链接消息用它的编码和编码包的校验）、`agent → transport`（链接的端点和安全通道）、`agent → platform`；`protocol → transport → crypto → util`，`protocol → media`（消息里的数据包类型）；`audio → platform → media → util`（AAC 来自平台，编解码接口在 `media/audio_codec.hpp`）。`seams` 是创建机器相关对象的地方（采集、注入器、输入权限、桌面状态、Ctrl+Alt+Del：平台的，测试可以替换），agent 和 session 经由它使用平台。`transport/local_stream` 在 Windows 上借用 `platform/windows/win.hpp` 这个只有头文件的工具（句柄、文字转换），不调用平台层的功能。平台层通过 `platform.hpp` 中的接口隔离：`ScreenCapture`、`VideoEncoder`、`VideoDecoder`、`InputInjector`，以及声音的 `AudioCapture`、`AudioPlayer` 和 AAC 编解码器，另外每个平台列出自己的编解码实现（`codecImplementations()`）并说明自己的后端（`codecBackend()`）。能力掩码和协商都从这份实现列表推导。新增平台，就是在 `src/platform/` 下新增一个实现目录。

日志的归属（`util/log`）跟着线程走：每条日志属于写它的线程所服务的会话、助手或看护者。C 接口在对一个对象的调用期间设置它（`log::Scope`），会话的工作线程从自己的开始；SDK 启动的线程用 `log::thread` 继承启动它的线程的归属；交给系统、由系统在自己线程上调用的回调（声音采集的数据和状态、播放的状态）在交出时用 `log::bound` 绑上交出者的归属。所以新代码用 `log::thread` 开线程、交给系统的回调用 `log::bound` 包一层，日志就记在对的对象名下；`session_logs` 测试要求一次运行里每条记录都属于某个会话，漏掉的线程会在那里暴露出来。判断一条日志要不要写，是一次线程局部读加两次原子读。

## 接口的三层与预设

公开接口分三层（取舍见 [API-DESIGN](API-DESIGN.md#4-决定) 第 36 条）。层次只是头文件和扩展的划分：代码都在核心库里，替换层没有另起库，C++ 包装照同样的头文件拆开。

| 层 | 头文件 | 内容 |
| --- | --- | --- |
| 入门 | `rcsdk.h` 的核心配置 `rcs_session_config`（`role`、`preset`、`connect`、`identity`）和 `rcs_session_callbacks`；`rcsdk.hpp` | 角色、预设、怎么连、怎么证明身份；生命周期、事件、取画面 |
| 功能 | `rcsdk.h` 的功能扩展；`module.h` 和 `modules/` 的头文件 | 每个功能一个扩展，只放宿主的决定 |
| 调优与替换：调优 | `tuning.h`（`tuning.hpp`） | 三个按领域的调优扩展：`RCS_EXT_VIDEO_TUNING`、`AUDIO_TUNING`、`NETWORK_TUNING` |
| 调优与替换：替换 | `host_codec.h`、`rate_control.h`、`host_transport.h`、`gpu.h`、`agent.h`（各有 `.hpp`） | 宿主接管的一环，各自一个扩展，回调和 `user_data` 在扩展里 |

替换层的扩展和它们的代码：

| 扩展 | 代码 |
| --- | --- |
| `RCS_EXT_HOST_ENCODER`、`RCS_EXT_HOST_DECODER`，画面来源 `ENCODED` | `agent/host_encoder`、`session/host_decoder`、`session/host_sources`（见“扩展点”） |
| `RCS_EXT_ENCODED_OUTPUT` | 两端。分享端：`session/share.cpp` 每个视频包发出之前交给 `on_video`，`session/audio_stream.cpp` 的 `AudioSender` 每个声音包发出之前交给 `on_audio`（带观看端的编号）。观看端：`session/view.cpp` 的解码循环在解码之前交给 `on_video`（接收队列重新开始的关键帧带 `after_gap`），`AudioReceiver` 在解码之前交给 `on_audio`；`decode` 0 时不开解码器、声音只交包和计数，`session/negotiation.cpp` 的 `offerOf` 按宿主的列表报格式（`session/checks.cpp`、`config.cpp` 核对） |
| `RCS_EXT_RATE_DECIDER` | `Session::rateReport`（`protocol/rate_control` 的 `propose` 和 `settle` 之间） |
| `RCS_EXT_HOST_TRANSPORT` | `transport/host_channel` |
| `RCS_EXT_GPU_DEVICE` | `session/view.cpp` 把设备交给平台的解码器（`platform::retainHostDevice`），`session/checks.cpp` 核对它 |
| `RCS_EXT_AGENT_LINK` | `src/agent/`（`remote`、`agent_server`、`link_messages`） |

**配置怎样解析**（`session/config.cpp`）：`kinds` 表每种扩展一行：类型、名字、角色、解析函数、在 `SessionConfig` 里的位置，以及预设给不给它值（`preset`）。所有处理扩展的地方都按这张表走，新类型漏不掉。

- `parseConfig`：核对核心配置（`checkConnect`、`parseIdentity`），`presetSettings(preset, config)` 先填好预设的值，再由 `applyExtensions` 把链里的扩展解析在上面，并把链里出现过的类型记进 `SessionConfig::chained`（`1u << type` 位）。
- `presetSettings` 是每个预设的取值：先放各个 `*_INIT`（就是 `BALANCED`），再按预设改几项。只有 `SHARE_VIDEO`、`ENCODER`、`SEND_AUDIO`、`VIEW_REQUEST` 和三个调优扩展有预设的值。取值是建议值，还没在实机上逐个量过（[API-DESIGN 第 6 节](API-DESIGN.md#6-预设的取值)）。
- `rcs_session_update` → `Session::update` → `applyExtensions`：同一个解析，`chained` 再加上这次的类型，所以放进去过的扩展从此是宿主的。
- `rcs_session_set_preset` → `Session::setPreset` → `applyPreset`：只给 `presetExtensions() & ~chained`（本角色的）换上新预设的值，然后和 `update` 一样经 `commit` 生效。
- `rcs_session_set_identity` → `applyIdentity`，并重新开始 PIN 的计数（`PinAttempts`）。
- `rcs_preset_defaults` → `presetDefaults`，`rcs_session_get_settings` → `getSettings`：都用 `writeSettings` 把 `SessionConfig` 里的结构体按宿主的 `struct_size` 复制出去，保留宿主的 `next`。
- 运行中能改什么由 `commit` 决定：`fixedExtensions`（替换层的 7 种）在运行中改是 `INVALID_STATE`，`turn_transport` 也是；`copyRuntimeSettings` 不复制它们。

**超时**（来自 `RCS_EXT_NETWORK_TUNING`）：`session/connect.cpp` 在每个连接或尝试开始时从当时的设置取。`connect_timeout_ms` 给 UDP 握手（`UdpOptions.handshakeTimeout`：观看端等回答，分享端的每个握手；P2P 和中继的分享端等对端开始握手是它的两倍）、TCP 连接和到 TURN 服务器的 TCP / TLS 连接；`idle_timeout_ms` 给 UDP 连接的存活（`UdpOptions.idleTimeout`，P2P、中继、HOST 也是这条连接）；`ice_gather_ms`、`ice_connect_ms` 给 ICE 的收集和连通检查；`turn_allocate_ms` 给 TURN 的分配。TCP 自己的问候（2 秒）、握手（5 秒）和心跳超时（5 秒）不变。

## 连接与协商

```text
建立路径 ──▶ 握手（临时 ECDH + 共享密钥）──▶ 双方同时发送 CodecOffer ──▶ 双方得出同一个编解码器
分享端：启动采集、打开编码器 ──▶ StreamAnnouncement ──▶ 视频包……
观看端：打开解码器（与分享端启动采集同时进行）──▶ 接收
```

“建立路径”取决于传输方式（`session/connect.cpp`）：

- UDP：直接发到配置的地址。
- TCP：观看端连到配置的地址，同一种数据报连接跑在这条 TCP 连接上（`transport/framed_socket` 的长度分帧：每个数据报前面 2 字节长度，最大 16 KiB）；分享端的端口是 `protocol/tcp_demux` 的 `TcpDemux`（见下面的“连接与协商”和“路径和迁移”）。
- HOST：宿主自己的通道。观看端（`transport/host_channel`）：`HostChannel` 属于一次运行：宿主从任何线程交进来的数据报（字节流先按 2 字节长度拆开）进它的队列，`HostSocket` 把它表现为一个只有一个对端的 `DatagramSocket`（Windows 用自己的事件、其他系统用一对管道唤醒等待中的网络线程），发出的数据报交给宿主的 `on_transport_send`（`RCS_EXT_HOST_TRANSPORT`，路由号 0；字节流前面加长度）；上面跑的就是 UDP 连接，经 `hostLink`，`connect_timeout_ms`（默认 5 秒）没有回应就失败；每个连接尝试有自己的 `HostSocket`，宿主调用 `transport_closed` 时先读完之前交进来的数据报（对端的告别可能在里面），再以 `NETWORK` 结束。分享端（`protocol/route_demux`）：`RouteDemux` 按宿主的路由号分路，路由号被当成一个不存在的地址（`100::/64`，RFC 6666 的丢弃前缀，路由号在后 64 位），所以 `UdpAcceptor` 像在 UDP 端口上一样并行做所有路由的握手（16 个、Retry cookie、过期报告），完成的连接得到只收自己路由的数据的套接字（`routeLink` 发回那个路由）；握手类数据报总进握手套接字（同一路由上重新连的观看端），没有连接的路由上的数据按“未知连接”回答。每个路由的字节流各自分帧，`transport_closed(route)` 只结束那个路由的连接（同样先交出关闭前到的数据），`transport_restarted(route)` 只告诉它。上限：连接 8 MiB，等握手的一个路由 64 KiB、合计 1 MiB、最多 1024 个路由，超过的 `QUEUE_FULL`。数据报大小按连接设置（`UdpOptions`）：UDP 路径 1200，宿主通道取宿主给的值，接收方收 65535 以内的任何大小。
- FORWARDER：两端都向转发器（`server/forwarder/`，单独的库和程序）发起一条连接（那一跳：同一个 UDP 连接，握手标记 `RCSFWD`，转发器出示自己的身份密钥，端点按 SHA-256 核对后才发令牌），跑在 UDP 上，或者跑在一条 TCP 连接、一个 WebSocket 上（`transport/framed_socket`：字节流上的数据报，TCP 入口每个前面两字节长度，WebSocket 每个一条二进制消息，`transport/websocket`；`wss://` 的 TLS 由转发器前面的代理终结，端点用系统的 TLS 客户端），或者跑在宿主给的通道上（`HOST`：那一跳的数据报经 `on_transport_send` 和 `rcs_session_transport_receive`，一次运行一个 `HostChannel`，路由 0；浏览器包就是这样，页面的 WebSocket 连 WebSocket 入口），由宿主的 `link` 选，不自动换。分享端（`session/forwarder.cpp` 的 `ForwarderLink`，每次运行一个）：那一跳上的“管道”（每个观看端的端到端数据报，前面带它在房间里的编号）交给一个 `RouteDemux`，`UdpAcceptor` 在上面像 HOST 一样做每个观看端的端到端握手，之后的准入、控制、数据、模块、声音都和直连一样；画面每张、声音每包用流密钥封装一次（`protocol/media_keys`，每个关键帧推一步），作为那一跳的视频和数据报上传一次（声音按格式每种只编一个，`session/audio_stream` 的 `SoundGroups`）；那一跳的动态码率量出上行，各档的码率不超过它；转发器每 100 ms 报来每个观看端的路径（喂给群组策略），并在它当场把某人移到低档时告诉分享端。观看端（`ForwardedConnection`）：把那一跳和管道里的端到端连接合成一个 `Connection`：端到端的消息照常，那一跳来的封装画面用端到端收到的流密钥打开后当作普通的视频包交给会话。转发器：每个入口一个 `UdpAcceptor`（UDP 入口在 `UdpDemux` 上；TCP、WebSocket 入口在 `RouteDemux` 上，每条连接一个路由，一个线程服务一个入口的全部连接，`server/forwarder/src/entry.cpp`），每个端一个读线程（第一条消息是令牌），一个 100 ms 的轮次线程（路径报告、当场下移、清理不报到的端）；多个房间，每个房间为每一路每一档留最近一个关键帧和之后的封装画面，晚来的、换档的观看端密钥打得开就从它开始，否则转发器向分享端要关键帧；每个观看端那一跳自己重传、加冗余、节流、做动态码率。
- P2P：ICE（`transport/ice.cpp`）。先收集候选地址，通过宿主交换信令，再做连通性检查、由观看端提名，选出一条双方都能通的 UDP 路径。观看端先给信令；分享端的 `IceListener`（`session/connect.cpp`）从运行开始到结束为每一份观看端信令单独做一次 ICE：在从 `connect.port` 往上第一个空闲的端口上收集（中继：自己的分配；收集最多 `ice_gather_ms`，连通检查最多 `ice_connect_ms`），经 `on_signal` 带着宿主的路由号交出回答，检查、握手，完成的连接交给 `Acceptor`，之后和 UDP 上的观看端一样。对端信令里的地址都会被探测，`ice::Agent` 因此剔除没有对端会在的（未指定、组播、广播、保留、云元数据服务），环回和链路本地的只在本端自己也有同一范围的候选时才用（`ice_addresses` 可以给出本机的环回地址），同一个 IP 最多 4 个，也不回答、不学习来自剔除地址的检查。`SignalQueue`（`session/signals.cpp`）控制节奏：同时最多 32 次，其余排队（最多 256 份），重复的信令跳过；分享端的信令写明它回答的是哪一份，观看端的连接据此只取自己的那份（`Answers`）。
- RELAY：同样走 ICE，但候选地址只有在 TURN 服务器（`transport/turn.cpp`）上分配的中继地址；候选的地址族是中继地址的（经 IPv6 到达的 coturn 也可能分配 IPv4 中继地址）。到服务器可以是 UDP，也可以是一条 TCP 连接（`transport/turn_tcp_socket.cpp`，`transport/framed_socket` 的一种分帧：把 STUN 消息和信道数据按自带长度装进 TCP 流，对上层表现为一个只有服务器这一个对端的数据报套接字 `DatagramSocket`，TURN 客户端、ICE 和 UDP 传输层不用知道区别；发送从不阻塞，连接一时收不下的最多先存 64 KiB，再多就像满了的 UDP 套接字一样丢弃）。中继赖以工作的请求（Refresh、CreatePermission、ChannelBind）被拒绝或 15 秒没有回答时，本次连接失败，原因写明请求和服务器的错误码：凭据类（401、403、441，例如有时效的凭据过期）是 `AUTHENTICATION`，其余是 `NETWORK`；“nonce 过期”（438）换新 nonce 重发一次，重发的又是 438 也算失败（否则路径上看得见请求的人能让它一直重发）。分配按服务器给的有效期过一半时刷新，至少隔一秒（不给有效期的服务器不会让它每拍都刷新）。配了用户名时服务器必须先要凭据：没要凭据就批下的分配不接受（路径上的人也能这样回答），到 `turn_allocate_ms` 以 `AUTHENTICATION` 失败并说明；不认证的服务器用空用户名。本地端口上还留着分配时（437：本端刚释放、服务器还在删，coturn 要一秒左右；或上次运行没释放就断网、崩溃），这个分配只可能是本端的，于是先释放它再重新分配；释放只发一次（coturn 每收到一次就把删除往后推），`turn_allocate_ms`（默认 5 秒）内拿不到新分配就失败，原因写明 437 和释放被拒的理由。

在选出的路径上（每种传输都一样），`protocol::UdpConnection` 提供 `Connection` 接口：

- 控制消息可靠、有序。
- 每路视频（`rcs_streams_config`，最多 4 路）各自给帧编号，各自按顺序交付、补发和放弃：分片、冗余片、补发请求（NACK）和“已不在了”（GONE）的头里都有路号，所以一路丢了包，别的路照常交付，不等它。
- 视频分片按 64 片一组发送，每组后面跟着这组的 Reed-Solomon 冗余片（`protocol/fec.cpp`：GF(2^8)、柯西矩阵的系统码，任意 k 片恢复 k 片数据；冗余片数写在每个分片的头里，冗余片的头里还有组号和这组最后一个分片的长度），冗余比例由分享端的网络线程按观看端的报告定（`AUTO`）或按宿主给的（`FIXED`），加冗余时分片的数据少 3 个字节，好让冗余片也装进同样大的数据报。组里缺的不超过到了的冗余片时当场补回；有冗余的组，第一次补要等这组的冗余都过去了（最后一个冗余片到了、或者后一组、后一帧的分片到了，再加乱序余量），免得为冗余能补的分片多要一次。冗余片不重发，整帧补要也只重发数据分片。观看端第一次补要一帧时，传输层交给会话一个本地的 `VideoMissing`，会话马上发 `RefLost{after, 还可能到}`（after 是它收到的上一帧）；传输层放弃这帧（`StreamGap`）时，接收队列保留丢失之前的帧，丢掉之后的，直到关键帧或者带 `recoversAfter ≤` 它保留的最后一帧的包，并再发 `RefLost{after, 放弃}`。分享端把各观看端最早的 after 交给助手（链接上的 `InvalidateAfter`），能做的编码器（NVENC：`nvEncInvalidateRefFrames`，按时间戳去掉它记着的最近 5 帧里 after 之后的那些，DPB 设成 5 帧）下一帧不参考它们，包里写上 `recoversAfter`；做不到、或者 after 已经比它记着的都早，就出关键帧。丢失的分片按需重传：分片按顺序发出，所以低于已到最高序号的空缺立即（3 ms 乱序余量后）补要，一帧的其余部分等后一帧的分片到了、或者按最近的到达速率早该到了才补要，慢速路径上不会把还在路上的分片当成丢失。分享端不重发还在队列里的分片、也不重发一个往返内刚发过的；已经不保存的帧（发出后超过 1 秒，或随积压丢弃）回答“已没有”，观看端立即放弃它。补不回来时产生 `StreamGap`，观看端从下一个关键帧恢复。
- 视频、控制消息和可靠数据是三条各自有序的流；声音和不可靠数据是各自独立的数据报。
- 发送节流和动态码率（`protocol/rate_control.cpp`）只在这条传输上生效。每份报告先更新测量、由规则（`RateRules`，来自 `rcs_video_tuning_config`）给出建议（`propose`），会话的 `rateReport` 把它连同测量交给宿主的 `on_rate_report`（`RCS_EXT_RATE_DECIDER`，不持连接的锁），最后的决定再限制在上下限里生效（`settle`）。规则看的排队是最近 `overuse_persist_ms` 里各份报告的最小值（无线电打盹一次的排队不算，持续的才算），丢包是最近 1 秒的；报告带着分享端最近实际发出的速率（编码器输出的平滑值），发得少于目标一半时排队不算过载、过载后也不上调；一次过载事件（相隔 2 秒内的算同一次）记下开始时的码率，恢复时快升到它的 90%，在它之上 10 秒没有过载就重新慢启动。发送节流是令牌桶：报告按规则的阈值算通畅时，每放进一帧就把排队的视频定在 `frame_spread_us` 内发完（不低于下面的慢速、不超过 `max_pacing_bps`），桶深一毫秒的量，所以小于这个量的帧一次发出；不通畅时按慢速（动态码率：目标的 1.5 倍且不低于编码器实际输出的 1.25 倍；固定码率：4 倍且至少 20 Mbit/s；都加上声音），桶深 2 ms。观看端对每一帧看第一簇（相隔不到 50 µs 一起到的分片：无线接收端的一个聚合帧）之后的部分：到达比发出明显慢，是路径瓶颈的速率（这些字节除以第一簇之后的时间）；没被摊开而分享端是节流着发的，路径至少能走它发出的速率；每份报告带上两种里各自最快的一个。分享端取最近一秒被摊开的那些的上四分位数（`PathRate`），通畅时每帧先发出相当于路径 `path_burst_us` 的量，其余最快按它的 `path_pacing_percent` 发（不低于慢速），大画面因此在分享端排队，不在接入点的队列里挡住声音。声音和不可靠数据在任何两个视频数据报之间插队，声音的数据报带发出时间，收端由此算声音在路径上的排队。收端的 `StallWatch`（`protocol/stall_watch`）看当前路径上到的每个数据报：30 ms 以上没有东西到、而之后到的那个在路径上排了至少一半时间（视频、冗余、声音带的发出时间，相对最近 10 秒的最低单向延迟），或这期间发的数据报丢了两个以上，算一次短断流；最近 10 秒的断流里大多数落在同一个节拍（或它的整数倍：漏掉的拍）上时，那就是节奏（无线电每 1.05 秒被 AWDL 拉走一次那样）。会话每 250 ms 发布路径时把它写进统计，节奏开始、变了一成以上或消失时发 `RCS_EVENT_STALLS`，并用 `PathStalls` 消息告诉对端（[指南 09](guide/09-transports.md#wi-fi-周期性卡顿awdl)）。可靠数据在发送线程里排在视频之后，并有自己的拥塞窗口（见“数据通道”）。网络线程一轮封好的数据报（节流这时放行的，最多 64 个；回 PONG 时立即）按顺序一起交给 `Link::sendBatch` → `DatagramSocket::sendBatch`：Linux（和 Android）的 UDP 套接字一次 `sendmmsg`，同一地址、一样大、标记相同的一串用 UDP GSO，DSCP 是每个消息的控制数据，系统拒绝 GSO 时那个套接字改成不用并记一次日志（`transport::DatagramBatcher`）；Apple 的系统一批在套接字的锁下逐个 `sendto`；Windows 每封好一个就发一个；其他路径（中继、TCP、宿主通道）逐个交给链路。
- 只有对端能发出认证过的帧，所以格式不对、类型不认识、帧号超出窗口的帧都结束连接，原因写明是哪种。

- 分享端在一次运行中一直监听（TCP 和 UDP），同时服务最多 `max_viewers` 个观看端（`RCS_EXT_VIEWERS`，默认 64）：UDP 的端口由 `UdpDemux` 分路（读线程按数据报类型和连接编号转交，不做密码学：握手类的数据报和没有连接的编号的数据进握手套接字，`UdpAcceptor` 在上面并行做握手、每个握手按当时的设置读凭据，完成的观看端得到一个只收自己数据的 `DemuxSocket`，它在有数据时直接唤醒连接的 `Waiter`，不占额外的描述符）；TCP 的端口由 `TcpDemux` 同样分路（一个线程接受、读、按连接编号转交、写出连接一时写不出去的部分，一条 TCP 连接对连接来说是对端的一个地址），上面跑同一个 `UdpAcceptor`。通过握手的连接交给自己的加入线程：收观看端的格式和设置，判断能不能服务（满员、格式、尺寸、声音：不能时回 `Refused` 带 `BUSY`），问 `on_viewer_join`，回答自己的格式。宿主之后再决定（`RCS_ADMIT_LATER`）时，加入线程在 `ViewerSet::waiting` 里登记这个观看端，等 `rcs_session_admit_viewer` 的决定、超时或运行结束；期间它和观看端互发 `Admission`（分享端约每秒一条，带剩余时间，观看端每条都回），连接因此不会被当作沉默断开，观看端离开也会在下一轮读到；决定后回答格式，或回 `Refused`（`REFUSED` 带宿主的原因，超时 `TIMEOUT`）。等待中的观看端占着 `joining` 的名额，不在观看端列表里。P2P、中继上每个观看端的连接来自它自己的一次 ICE（见上面的 P2P），HOST 上来自它那个路由的握手（见上面的 HOST）。最后一个观看端离开后，默认再次监听（P2P、中继：进行中的 ICE 照常继续，新的信令照常回答）等下一个（`rcs_reconnect_config.keep_listening`），每个观看端从头协商。握手失败的连接（密钥不对、协议版本不同、连上后 2 秒不发握手消息、中途断开）会被丢弃并计入网络统计的 `rejected_connections`，分享会话继续等待。所以局域网里的端口扫描或错误密钥都结束不了分享会话。每 5 秒最多 16 次失败逐一报告给宿主（计数和日志），其余汇总成一条日志，洪水填不满宿主的日志。
- TCP 的端口上，还没归属任何连接的 TCP 连接同时最多 16 条（再来一条，最早的那条关掉），连上 2 秒还什么都没发的、发过东西但握手超时（`connect_timeout_ms`）再过 2 秒还没归属的都关掉，计入 `rejected_connections`；握手和 UDP 上一样由 `UdpAcceptor` 并行做（16 个、Retry cookie），所以发了问候就不说话、或者根本不说话的连接挡不住真正的观看端。一条 TCP 连接归在它上面完成握手的连接，以及往它上面发过东西的连接（连接只往认证过的数据来的地方发，没有密钥的人让它发不过去）；一个连接最多 4 条，多出来的，连接最久没往上发过东西的那条关掉。连接结束时，只归它的 TCP 连接写完最后的话（关闭帧）、对端也关了（最多 1 秒）就关。每条 TCP 连接写不出去的最多先存 256 KiB，再多就像满了的 UDP 套接字一样丢掉。
- UDP 直连时分享端同时处理最多 16 个握手。已满时，新来的问候（包括来自已有握手地址的新问候）只会收到一个 cookie（Retry），分享端不为它保留任何状态，也不生成密钥。观看端带上 cookie 重发，证明它能在自己的地址上收到回复，然后顶替它自己先前的或最早的那个未完成握手。问候先检查格式和协议版本，再生成临时密钥。所以伪造源地址的握手洪水占不满分享端，真正的观看端照样能连上。PIN 问候不论满不满都先拿 cookie：回答它要占 PIN 的一次尝试，伪造源地址的问候不能把次数用掉。问候补零到和 `ServerHello` 一样大（`0xD1 问候 cookie(16) 零`，还没有 cookie 时那 16 字节是零），所以对没证明过地址的来源，回答（`ServerHello`、Retry、版本通知、PIN 通知）都不比它发来的大，伪造源地址也放大不了流量；没补齐的问候不回答。P2P 和中继只接收选定路径上对端的数据，不受影响。
- UDP 握手消息本身没有认证，谁都能冒充对端的地址发一个：分享端收到校验不过的 finished 只记下，这次握手仍等真正的那个，过期（问候后 `connect_timeout_ms`，默认 5 秒）时才报告失败；观看端收到校验不过的答复（另一个密钥、不信任的设备密钥、另一个协议版本）也只记下，继续重发问候，1 秒内没等到有效答复才以这个失败结束；PIN 例外：一个问候只验证第一个走到 PIN 校验的答复（每验证一个就是替回答者猜一次），它不对就立即以 `AUTHENTICATION` 失败，之后的答复一概不验证。P2P 和中继的分享端两倍 `connect_timeout_ms`（默认 10 秒）没等到对端完成握手时，失败原因里带上最后一次失败的原因。

- `CodecOffer` 是握手后的第一条消息，内容是宿主在 `rcs_encoder_config` / `rcs_decoder_config` 的 `codecs` 中列出、且本机在所配置的加速方式（硬件或软件）和后端下有实现的编解码器，按宿主的顺序排列，另有本端两个方向的声音格式（发的、收的）、现在发不发和收不收，以及平台。默认只列 H.264。一个都不支持时 `start` 直接返回 `UNSUPPORTED`，不会连接。SDK 从不添加宿主没列出的格式；宿主列出第二个格式，就表示它接受这个替代。宿主运行中改了这些设置时，本端再发一次 `CodecOffer`，对端据此重新规划并发出 `RCS_EVENT_PEER`。
- 规则：**分享端列表中第一个、观看端也提供了的编解码器**。两端用同一规则独立得出结果，不需要额外往返。没有交集时两端都以 `UNSUPPORTED` 失败，错误信息写明双方各提供了什么，`rcs_peer_info` 也给出对端的列表，宿主可以据此决定是否换一组格式重连。
- 两端先发后收，所以协商不增加一次往返。观看端知道结果后立即打开解码器，和分享端启动采集、打开编码器同时进行；早到的数据包留在套接字缓冲里，不会在接收队列中超龄。
- 观看端紧接着发 `ViewerSettings`：它要求的画质和声音（`rcs_view_request_config`）和指针模式；设置变了就再发一次（收不收声音在 `CodecOffer` 里）。分享端收到第一条之后才定下画质（`session/stream_plan.cpp`：观看端要求的字段在策略上限内照办，其余用分享端自己的设置），再开始采集和编码。
- `StreamAnnouncement` 在第一个视频包之前发送，带编解码器、尺寸、帧率、码率、分享端的加速方式、画面在编码帧中的位置，以及共享的东西（显示器、区域、窗口）的像素尺寸和刷新率。画质变化、采集在显示模式变化后恢复、共享的窗口改了大小、画面位置变了时，分享端会在下一帧之前再发一次。观看端收到自己没有提供的编解码器时视为协议错误。
- 指针单独传送时，分享端发 `CursorShapePart`（指针图像按 32 位像素做游程压缩，分成每条不超过 900 字节的控制消息；同一图像在本次运行中只发一次，再出现时沿用编号）和 `CursorPosition`（显示、热点位置、形状编号，变化时才发）。
- 网络协议的版本在握手第一条消息的协议标记里（`transport/handshake.hpp`，现在是 v1），线上格式每变一次就加一。1.0 之前不和其他协议版本通信：标记对不上时，分享端回一个只含自己协议标记的通知（UDP 上它不大于对方的问候，每 5 秒最多 16 个，不会被用来放大流量），丢弃这个连接并计入 `rejected_connections`；观看端以 `RCS_ERROR_UNSUPPORTED` 失败，原因里写明双方的协议版本。

## 数据流

**分享端的一次运行**（`session/share.cpp`）：`ShareRun` 的循环线程（每 2 毫秒一轮，用 `sleepPrecise`：Windows 上普通的限时等待会拖到 15.6 ms 的时钟周期）管接收（`Acceptor`）、加入线程、观看端的加入和离开（离开的在自己的线程上销毁，告别不阻塞循环，停止时所有观看端同时告别）；`ShareStream` 是这次运行的画面（控制授权、路径和动态码率、文本插入点，以及每一路的 `Lane`：计划、编码器、分到的码率、关键帧、丢失的画面、公告和指针），第一个观看端加入时创建、最后一个离开时结束。有观看端要的路才在助手里开采集和编码（助手每路一个 `VideoPipeline`，各自的线程），没人要的路停掉并告诉观看端（`StreamEnded`）；总码率按权重分给各路，静止的路让出份额；指针在最后看到它的那一路的显示器上。每个观看端（`session/share_viewer.hpp` 的 `Viewer`）有自己的连接和取消标志、读线程（输入、关键帧请求、设置和格式、数据）、发送线程（`Outbox`：流公告、视频、指针、桌面状态、控制授权按顺序发，视频有界：积压超过 1 秒就丢、等关键帧并要一个）、声音编码（`AudioHub` 只采集一份，分给每个观看端的编码器）、数据队列和统计。助手的每个视频包只编码一次成网络消息，`ShareStream` 在 `ViewerSet` 的共享锁下把同一个消息交给每个观看端的发送队列（不做 I/O）；加入和移除在独占锁下，销毁在移除之后，所以分发不会碰到正在销毁的观看端。采集的反压取最快的观看端的积压。

**分享端**分成会话和助手（`src/agent/`）。会话决定要什么（画质、码率、关键帧、声音、输入设置），助手在这台机器上做（采集、编码、声音采集、输入注入），两边只通过 `agent::Port`（会话的要求）和 `agent::Events`（助手的报告）往来：

```text
助手（MediaAgent，自己的线程）                                  会话（ShareLoop、连接）
ScreenCapture.next ──▶ VideoEncoder.submit/receive ──▶ Events.videoPacket ──▶ PacketSender ──▶ Connection.send
      ▲                       │（异步硬件编码）          Events.videoStarted ──▶ StreamAnnouncement
      └── 编码完成时 interrupt()，唤醒等待中的采集        Events.cursor ──▶ CursorSender
AudioCapture / 麦克风 ──────────────────────▶ Events.audioPcm ──▶ 编码（Opus / AAC / PCM）──▶ Connection.send
InputInjector.apply ◀── Port.applyInput ◀── 宿主 on_input ◀── 观看端消息线程：Input
VideoEncoder.requestKeyframe ◀── Port.requestKeyframe ◀── KeyframeRequest、rcs_session_request_keyframe、keyframe_interval_ms
采集等待 ◀── Port.setBacklogSource ◀── Connection.sendBacklogNs（发送积压约两帧时不采集）
```

- 助手默认就在会话的进程里（`MediaAgent` 直接实现 `Port`，事件是直接调用）。挂了 `rcs_agent_link_config` 时，会话一侧是 `RemoteAgent`：它在本机端点上等助手进程连上来，把 `Port` 的调用变成链接消息，把助手的报告变回 `Events` 调用；助手一侧的 `AgentServer` 收到消息后驱动它自己的 `MediaAgent`。两种方式用同一份 `MediaAgent`，所以行为一致。`RemoteAgent` 记着会话当前要求的画面、声音和输入设置，新助手连上时重新交给它；助手离开时向会话报告“采集暂停、输入不可用、声音暂停”。链接上只走编码后的画面、PCM、光标和控制消息。
- 一条链接的建立（`agent/remote.cpp`、`agent/agent_server.cpp`、`agent/link_messages.hpp`）：
  - 端点（`transport/local_stream`）在接受时就向系统问对方是谁：进程号、Windows 会话号、账户（Windows：命名管道客户端以识别级模拟得到的令牌；其他系统：`getpeereid` / `LOCAL_PEERPID` / `SO_PEERCRED`），以及是否特权（LocalSystem、令牌里管理员组已启用；root）。助手一侧同样问服务端点的是哪个进程（Windows：`GetNamedPipeServerProcessId` 和它的令牌，读不到令牌时用管道的所有者，只有创建者能设置它），按 `rcs_agent_config.access` 决定要不要理它（`PRIVILEGED`：自己的账户、LocalSystem / root、提权的管理员）。`rcs_agent_info` 里的进程号、会话号、账户都来自这里，不来自助手自己的消息。
  - 然后是明文的前缀：助手先发 `RCSLINK` + 链接格式号（`agent/link_messages.cpp`，现在是 1）+ 它的 `RCSDK_VERSION`，会话回自己的。前 7 个字节以后的版本也不变，所以不同版本在任何别的解析之前就互相认出：会话写日志（“助手是 X 版、会话是 Y 版”）并断开，助手以 `UNSUPPORTED` 结束、不再重试。不是链接的东西（比如网络客户端的握手）在这里就被丢掉。
  - 再是安全通道，用的密钥是 HKDF(链接密钥, "rcsdk agent link")：网络连接即使碰巧用了同一个 32 字节密钥，也打不开链接，反之亦然。助手发 `AgentHello`（机器能提供什么），会话先问宿主（`on_agent_attach`，内部是 `Observer::admit`，拿到的是系统报告的进程信息），接受才回 `Accepted`，否则回 `Refused`（`AUTHENTICATION`）并断开；被拒绝的助手按自己的重连间隔再试，日志只写一次。
  - 每个连上来的连接在自己的线程上做这些检查（这个线程之后就是它的读线程），接受连接的线程马上回去等下一个，Windows 上总有一个管道实例在等。同时检查的连接最多 16 个、同一账户最多 2 个；某账户的连接失败（前缀不对、握手失败、宿主拒绝）后，它的下一个连接等 250 ms，每再失败一次翻倍，最多 10 秒，等待期间的连接不读就关掉；连上就清零。被拒绝的连接写进日志时，一阵里只写第一条，其余每 10 秒汇总一次。所以一个连上就不说话、或者不停重连的本机进程占不住端点，也刷不满日志。
  - 新助手被接受后才替换旧的：旧的不再被听，它的写线程发出最后一条 `Replaced`，等旧助手关闭链接（读线程照读，最多 2 秒，`link::lastWordsWait`），旧助手收到后以 `STOPPED` / `RCS_ERROR_REPLACED` 结束，不管它的 `reconnect` 怎么设（否则旧助手会重连回来再把新的挤掉，两个都活着时来回替换）。会话不先关：先关的话，旧助手正在发送的线程先失败、取消链接，读线程可能还没读到已经到了的 `Replaced` 就把它当作链接断了，又连回来。
- 链接上助手的离开都在它自己的读线程上报告，从不在会话调用 `Port` 的线程上（那里可能持有会话的锁，比如控制授权的锁，宿主在回调里改控制设置就会死锁）：会话的命令积压到 8192 条（助手不收）时只给这个连接记下原因并关闭它，读线程醒来后报告。离开的原因进 `rcs_agent_info.result` / `message`：助手关闭了链接是 `RCS_OK`；助手报告的失败（`Failed`，比如采集跟不上新桌面）是它的错误码；链接坏了（超时、消息不合法、内容区域超出画面、不收命令）是 `RCS_ERROR_NETWORK`。
- 另一个进程里的助手失败时（`Events::failed`），它把原因发给会话，不再执行会话的命令，等会话关闭链接（最多 2 秒，道理同 `Replaced`：它先关的话，会话正在发送的线程先失败，报告的就成了发送失败而不是助手的原因），然后按重连设置再连上来，所有东西重新建立；会话把它当作一次离开，运行继续。本进程里的助手失败仍然结束运行。助手连上时对设置的检查（`Session::checkMachine`）不满足时，运行以那个原因结束，等观看端时也一样：`ShareAgent` 记下这个失败，正在等观看端时取消等待，`whileListening` 把取消换成这个失败；后来的失败不会被先前的掩盖，本进程助手在上一条画面结束后报告的旧失败在下一条画面开始时丢掉。
- 采集只保留最新一帧；编码器在途帧达到 3 时先等输出，不让延迟堆积。
- 会话的分享循环每圈先看两端的设置有没有变（宿主的运行中修改、观看端的 `ViewerSettings` 和 `CodecOffer`）。变了就重新计算画质；画质、采集设置或编码器设置不同时，让助手重新开始画面（助手先把旧编码器里的帧发完，再按新画质启动采集和编码器，从关键帧开始，回报实际的显示器和画面位置），会话据此发新的 `StreamAnnouncement`。助手回报的显示器使画质需要调整时（要求超过了显示器的尺寸或刷新率），再重新开始一次。只有指针模式变了时，不重启采集，只切换指针是否画进画面。码率限制和模式交给连接（`setVideoRate`），目标码率交给助手的编码器；显示器或输入执行者变了就换输入注入器（释放旧的按键）。助手每 2 秒核对一次显示器列表，变了发 `RCS_EVENT_DISPLAYS`；每秒核对一次系统是否接受注入的输入，控制授权跟着它暂停和恢复。
- 宿主自己提供画面时（`RCS_CAPTURE_HOST`），`HostVideoCapture` 实现同一个 `ScreenCapture` 接口：宿主推送的帧（复制、最新一帧替换上一帧）按流的帧率交给编码循环，其余流程相同；宿主推的 GPU 图像（`rcs_session_push_gpu_frame`）由平台核对（`platform::hostGpuFrame`：类型、格式和缓冲区的实际布局、尺寸、颜色）并持有，成为一个带 GPU 表面的 `VideoFrame`，走同一个槽，编码器直接用它；sample buffer（`rcs_session_push_sample_buffer`）的画面由 `platform::SamplePictures`（`apple/sample_buffers.mm`）在 GPU 上转正、按原来的形状放进流的尺寸，和它在帧里的位置、本来的尺寸一起放进槽，采集取走时 `content()`、`display()` 跟着变，视频管线照常重新公告；声音按种类（设备和应用的、麦克风）各由一个 `platform::SampleSound`（AudioConverter）转成 48 kHz 立体声，`audio::Mixer`（`src/audio/mixer.cpp`，可移植）按到达混成一路（快的等慢的、停了的不久等、一直快出来的去掉，按 `rcs_send_audio_config` 的两个音量，0 不要那一种），再和 `rcs_session_push_audio` 一样交付；Android 的硬件缓冲区（`rcs_session_push_hardware_buffer`）走同一个 `SamplePictures`（`android/frames.cpp`）：GL 线程把它当外部纹理转正、放进流，画进 SDK 自己的缓冲区池，函数返回前 `glFinish`，因为宿主放手后它的生产者会重用那块缓冲区；`rcs_session_push_gpu_frame` 收的 `AHardwareBuffer` 也这样复制；声音同理（`HostAudioCapture` 实现 `AudioCapture`，推送时直接交付），输入交给宿主时 `HostInput` 实现 `InputInjector`（`apply` 什么都不做，`releaseAll` 记下待通知，由 SDK 线程在不持锁时通过 `on_input` 送出 `RCS_INPUT_RELEASE_ALL`）。
- 指针单独传送时，采集不把指针画进画面，只有指针变化时不产生新帧；Windows 从桌面复制的指针信息得到位置和形状（`GetCursorInfo` 比对系统指针句柄判断标准类型），macOS 每次取帧时读取指针位置和是否被应用隐藏（`CGCursorIsVisible`）、每 50 ms 按像素比较一次 `NSCursor.currentSystemCursor` 的图像。指针图像最多 256×256 像素，更大的在两个平台上都等比缩小（热点跟着缩放）。
- 屏幕静止时没有新帧。如果观看端此时请求关键帧，就重新编码最后一帧。
- macOS 硬件编码优先使用低延迟码率控制（苹果的实时编码器，跑在硬件编码器上；如果它报告自己不在硬件上运行，就不用它）。它在提交调用里同步编码，一次一帧；如果前 30 帧显示提交阻塞超过帧间隔的 3/4，或者参数超出 H.264 Level 5.2，就改用可流水线的硬件编码器（每帧延迟高约 15 ms，但帧率够）。当前用的是哪种，见视频发送统计的 `low_latency`；切换时发出 `RCS_EVENT_CODEC`。编码器最多同时处理 3 帧。
- 共享什么由 `platform::CaptureTarget` 说（显示器、区域、窗口、应用），采集和输入注入器都按它来，它随视频流和输入设置一起经链接交给助手。采集报告的“暂停”带原因（`rcs_capture_pause`），窗口或应用看不到时也是暂停，不让运行失败。macOS：ScreenCaptureKit 的独立窗口过滤、按应用的显示器过滤和源矩形，窗口是否在屏幕上、是否还存在问窗口服务器（`platform/macos/window_server`），窗口的帧附带它在帧里的位置；Windows：单个窗口用 Windows.Graphics.Capture（`window_capture.cpp`，直接用 WinRT 的 ABI 接口，没有 C++/WinRT），区域和应用仍是桌面复制，由合成着色器只画源纹理的一块、或只画应用窗口在前的那些矩形（`window.cpp` 按窗口的 Z 序算出）。输入注入器按目标把 0..65535 映射到显示器、区域或窗口现在的位置；共享窗口或应用时，按下前先看那一点前面是谁的窗口，必要时提上来（`bringToFront`），按键只在它的应用有焦点时注入。
- 取图（`rcs_capture_picture`，`session/thumbnail.cpp`）：先按会话的采集检查（`checkCapture`，对象是本进程的机器），再交给平台的 `captureThumbnail`。在会话的助手那里取是缩略图模块（`modules/thumbnail`）的事：它的机器那一半在助手里调 `rcs_capture_picture`，像素作为模块消息回来（链接上按 4 MiB 分段的 `ModuleData`）。macOS 用 `SCScreenshotManager`，过滤器和流共用 `shareable()`；Windows 显示器、区域、应用用桌面复制、窗口用 Windows.Graphics.Capture 取一帧，`platform/windows/thumbnail.cpp` 用合成着色器按原尺寸画进带 mip 的纹理（HDR 换算、应用遮罩都在这一遍），生成 mip，再三线性缩到目标尺寸并读回。Windows 一个进程同时只能复制一个显示器一次，所以 `capture.cpp` 记着本进程正在复制的显示器：有运行中的采集就把请求交给它，它在下一次 `next()` 里、在自己的线程上（复制属于那个线程所在的桌面）用下一幅桌面图像取图，静止的桌面没有新图像时重新复制一次（新的复制立刻给出整幅桌面），这幅图像留给它自己的下一帧；没有就为这一张复制一次，期间开始的采集等它结束。
- 采集按额度限速（`ScreenCapture::pace` / `due` / `took`，所有平台和宿主推的画面）：额度按帧率增长、最多攒一帧半，一帧到了额度够就立刻交给编码器，不按固定的相位等；画面来源和流同频时（60 Hz 屏推 60 FPS）因此不会多等 0～1 帧。macOS 采集直接输出 NV12；ScreenCaptureKit 的最小帧间隔设为目标的一半，否则早到的帧会让它跳过一次刷新。Windows 编码是同步的：每帧在采集后立即编码，不在队列中等待。实测 4K 120 FPS 时 NVENC 每帧约 5.4 ms，循环周期正好 8.3 ms，同步方式足够；异步只能提高吞吐，不能降低延迟。
- 输入注入按类型分派：Windows 用 `SendInput`（按键是扫描码，媒体键是虚拟键，文本是 `KEYEVENTF_UNICODE`，相对移动不带 `ABSOLUTE`），触摸和笔用运行时加载的合成指针设备（`platform/windows/pointer_input.*`，`CreateSyntheticPointerDevice`，Windows 10 1809 起；每次把所有按着的触点放进同一帧）；macOS 用 CoreGraphics 事件（媒体键是系统定义的辅助键事件，文本是按键事件的 Unicode 字符串，笔是接近事件加带压力和倾斜的数位板子类型鼠标事件）。媒体键的对应表在 `platform/keys.hpp`。能执行哪些类型由各平台的 `capabilities().input_types` 报告，经助手的机器信息和编码格式提议到观看端。
- 按类型交给宿主的输入（`host_input_types`）在 `ViewerMessages::apply` 里分派：宿主的类型 `on_input` 之后就算执行了，不进注入器；`ShareControl::releaseAll` 同时释放系统的注入器和（有宿主类型时）通知宿主。
- 文件传输（模块 `modules/file_transfer`，没有核心的部分）：会话那一半有两端的传输表，清单、接受、256 KiB 的块、确认、条目结果、取消、暂停都是模块消息；每次传输最多 4 MiB 没被对方确认写好（窗口），每个对端的模块队列满了就排在模块自己的等待队列里等 `room`。机器那一半（分享端在助手里，观看端在本进程）一个线程做文件的事：列目录、读、写 `.rcsdk-part`、按冲突规则改名、设时间、查剩余空间、续传记录。断线（`peer_left`）或助手离开（`machine_left`）时传输等待，回来后接收方按“已收到”或“已写好”回答从哪里继续，纪元让旧的块作废。`target` HOST 时宿主的回调在会话那一半的一个线程上，结果经 `rcs_module_run` 交回；清单超时、等对端、限速、进度由会话那一半的一个节拍线程推动。设计见 [FILE-TRANSFER-DESIGN](FILE-TRANSFER-DESIGN.md)。
- 文本插入点（模块 `modules/text_caret`）：会话那一半按问候（观看端要不要、分享端告不告诉）和 `peer_control` 决定看不看；机器那一半在助手里，一个线程按间隔用读取器（Windows 是系统光标和 UI Automation，读取器和 UI Automation 都在这个线程上；macOS 是辅助功能）看，屏幕坐标按 `rcs_module_get_shared` 给出的区域换算成输入坐标，变了才告诉会话那一半，会话那一半再发给看着的观看端。`rcs_module_get_shared` 的区域来自 `MediaAgent` 最后一次 `configureInput` 的目标（主显示器在那时解析成编号），由 `platform::sharedArea` 换成屏幕坐标，和远程输入按同一块区域映射。
- Windows 编码器在 `open()` 时就绑定采集所用的 D3D11 设备，并只选择这块显卡上的硬件编码器（按适配器 LUID 枚举），双显卡机器上不会选错。
- Windows 有两个编码后端。Media Foundation 通过系统的编码器变换（MFT），任何厂商；NVENC 直接调用 NVIDIA 驱动的 `nvEncodeAPI64.dll`（运行时加载，接口声明来自 `third_party/nvenc`，MIT），在同一个 D3D11 设备上开编码会话。NVENC 编码器在 `open()` 时登记 3 个 NV12 输入纹理，每帧用同一个视频处理器把 BGRA 转成 NV12 写进其中一个，同步编码、锁定输出、拆成参数集和画面；码率变化用 `nvEncReconfigureEncoder` 在下一帧生效，不出关键帧。`codecImplementations()` 对每块 NVIDIA GPU 各探测一次（开一个编码会话，看它支持哪些格式）。驱动的模块在整个进程里只记一个接口版本，就是最后一次 `NvEncodeAPICreateInstance` 要的那个；NVIDIA 自己的 Media Foundation 编码器打开时会要它自己的旧版本，之后用我们的版本开会话就失败（`NV_ENC_ERR_INVALID_VERSION`），所以每次开会话前重新要一次，已经开着的会话不受影响（Windows 上实测两种后端交替打开、交替编码）。接口有两个版本：13.0（驱动 570 起，HEVC、AV1 也能时域分层）和 12.0（驱动 522.25 起）。13.0 改了好几个结构的布局（HEVC、AV1 的位深换成单独的字段，原来的位给了别的开关），一个头文件对不上另一个版本的驱动，所以同一份实现（`platform/windows/nvenc_api.inl`）按两个头文件各编译一次（`nvenc_api13.cpp`、`nvenc_api12.cpp`，头文件各放在自己的命名空间里），`nvenc.cpp` 取驱动讲得了的最新的那个，进程里只用它；用 12.0 时日志说一行。驱动讲得了它之前的所有版本，所以 `codec_roundtrip` 在新驱动上把 12.0 的路也跑一遍。
- Windows 的另两个系统后端。oneVPL（`platform/windows/onevpl.cpp`）：Intel GPU 上的编码器，直接调 Intel 显卡驱动带的运行时 `libmfx64-gen.dll`（按显卡驱动的 `DriverStorePathForVPL` 找，不带 oneVPL 的调度器），每路流一个会话、给它采集所在的 D3D11 设备；画面转成流的布局后拷进运行时给的输入表面，同步编码，每帧可带参考列表（参考帧失效）。NVDEC（`platform/windows/nvdec.cpp`）：NVIDIA GPU 上的解码器，只接 Media Foundation 在 GPU 上解不了的 4:4:4；它自己的码流解析器，解出的平面经 CUDA 和 D3D11 的互操作拷进纹理数组，着色器画成 BGRA8 纹理。oneVPL 也是 Intel GPU 上的解码器（H.264、HEVC、VP9、AV1，含 HEVC、VP9 的 4:4:4）：每路流一个会话、在解码设备上，第一个关键帧时按它的序列头建解码器（参数变了的关键帧再建），每个包作为完整的一帧送进去、同一次调用里出画面；运行时分配的画面（D3D11 纹理）4:2:0 照原样拷出它的池，4:4:4（AYUV、Y410）由 SDK 的着色器画成 BGRA8（`bgra_shader.hpp`，NVDEC 的平面也由它画）。三个硬件解码器（Media Foundation、NVDEC、oneVPL）交出的纹理都放在一个池里（`picture_pool.hpp` 的 `TexturePool`），宿主和会话的队列都不拿着了就再用：核显上新纹理第一次画进去很慢。解码器的工厂 `createVideoDecoder(config)` 和编码器的一样按配置选：指定的后端，或 `AUTO` 时 4:4:4 的流交给解码设备那块 GPU 上的 NVDEC（`nvdecFirst`），再是 oneVPL（`vplDecodesFirst`，Windows 没装某编码格式的解码器时它的 4:2:0 也交给 oneVPL）。
- Windows 采集在 GPU 上直接从复制出的桌面缩放、合成指针，画进一个小纹理池（宿主或编码器还拿着的纹理不复用）。HDR 桌面（scRGB）按该显示器的“SDR 内容亮度”换算成 SDR：SDR 白色映射为白色，更亮的部分截断；这个亮度每 2 秒重新读一次。

**锁屏、UAC 和显示模式变化**（被控端）：

- Windows：锁屏、UAC 弹窗切到安全桌面、改分辨率、开关 HDR 时，桌面复制会丢失（`DXGI_ERROR_ACCESS_LOST`）。采集不会因此失败，而是暂停：连接保持（心跳照常），观看端停在最后一帧，SDK 每 250 ms 尝试重新复制一次。桌面能捕获后自动恢复；画面在编码帧中的位置变了就重新发 `StreamAnnouncement`。显示器被拔掉也一样暂停（`DESKTOP`）：每次重试先按编号找这块显示器，它回来了、或者系统的输出变了（DXGI 工厂的 `IsCurrent` 为假），就重新找它的输出再复制；输出必须还在采集开始时那块显卡上（采集不换设备，见下面远程桌面一条），换到别的显卡上就以 `RCS_ERROR_CAPTURE` 失败、写明要重新开始采集；断开 10 秒以 `RCS_ERROR_CAPTURE` 失败（“The shared display was disconnected”），和 macOS 一样。安全桌面和锁屏只有 LOCAL_SYSTEM 进程能捕获，所以这期间对方看不到 UAC 弹窗本身。采集统计的 `paused`、`interruptions` 和 `RCS_EVENT_CAPTURE` 事件让宿主看到这一过程。
- 远程控制同理：每秒检查一次系统此刻是否接受注入的输入（`platform::inputAvailable`。Windows：输入桌面是不是普通桌面，跟随安全桌面时是不是打得开；macOS：辅助功能权限）。不接受时暂停授权并释放按住的键，接受后自动恢复；每次变化都换授权代次。宿主的“允许控制”设置不受影响，`rcs_control_info.granted` 反映当前是否真的能控制。单个事件被系统拒绝（例如点到了以管理员身份运行的窗口）只释放按键，不撤销授权。
- 输入权限（`platform::inputPermission`、`rcs_input_permission`、`rcs_agent_info.input_permission`）和这个“此刻”分开：它只说进程能不能注入。Windows 上用户会话里的进程是 `GRANTED`，会话 0 的进程是 `UNAVAILABLE`（服务没有桌面，也没有能授予的东西）；macOS 是辅助功能权限。设置的检查只看权限，所以锁屏时连上的助手、锁屏时打开的控制都不会被拒绝，只是授权暂停到解锁。
- Windows 上旋转的显示器：桌面复制交出的纹理是面板上未旋转的图像（桌面 768×1024、转 90 度时是 1024×768 的纹理，桌面在里面转着），`DXGI_OUTDUPL_DESC.Rotation` 说明转了多少；`ModeDesc`、显示器的位置、指针的位置和形状都是桌面方向的（微软的 Desktop Duplication 文档和示例，WebRTC 也这样用）。所以区域、应用遮罩、指针叠加和输入都按桌面方向算，只有着色器取样时按旋转找纹理里对应的点。纹理的形状和当前模式对不上时（模式正在改变、还没丢失复制）当作复制丢失，重新复制。
- 输入坐标每次都按显示器当前的位置换算，分辨率或布局变化后仍然准确。
- 显示器编号（`rcs_display_info.display_id`）在显示器保持连接期间不变，拔掉别的显示器、换主显示器都不影响：macOS 是 `CGDirectDisplayID`，Windows 是 GDI 设备名 `\\.\DISPLAYN` 中的 N。0 表示主显示器，采集和输入各自在开始时确定是哪一个并一直用它。
- 开始采集时（观看端连上之后）桌面就不可捕获，例如已经锁屏，本次运行会失败并说明原因；只有已经在采集的会话才会暂停。
- 重新复制时 `DuplicateOutput1` 返回 `DXGI_ERROR_UNSUPPORTED`（桌面处在复制不支持的模式）也按暂停处理：微软的文档说这时等桌面切换或模式变化后再试。开始采集时遇到它仍然失败。
- **远程桌面会话里的采集**：只用桌面复制，不换别的采集方式。微软的桌面复制文档没有说远程桌面会话里支持还是不支持；它的错误码里有远程桌面专用的一个（`DXGI_ERROR_SESSION_DISCONNECTED`：“会话现在断开了”，`DuplicateOutput` / `DuplicateOutput1` 的返回值说明），按暂停处理。微软问答（Q&A 1068807，2022 年，回答的微软支持人员说复现过）报告：Windows 10（1909、21H2、22H2）上第一次远程桌面连接时能复制，客户端断开再重连后复制失败（`E_INVALIDARG`），Windows Server 2016 / 2019 上没有这个问题；Windows 11 没有资料。SDK 不在采集内部换设备重来：编码器是在采集的 D3D 设备上打开的，换显卡设备等于换编码器，要整条管线重来。所以复制在远程桌面会话里失败时，采集以 `RCS_ERROR_CAPTURE` 结束，原因写明是哪个远程桌面会话、哪个 `HRESULT`、要重新开始采集；重新开始的采集（新的一次运行，或者重新连上来的助手）会重新枚举显卡和输出、新建设备。看护者守着远程桌面会话时根本不跨过重连：客户端一断开它就结束被看护进程，重连后启动新的。
- macOS 被控端同样暂停而不失败：显示器睡眠或黑屏（帧状态如此）、流停止（锁屏时 ScreenCaptureKit 不列出任何显示器）、显示模式变化（重新开始流，让画面重新放置）时暂停，每 250 ms 尝试重新开始流，恢复后按新的画面位置重新发 `StreamAnnouncement`。显示器断开 10 秒、用户在系统的屏幕共享控件里停止、隐私设置不再允许，或流 10 秒内一直起不来时，本次运行以原因失败。

**跟随安全桌面**（`rcs_capture_config.secure_desktop`，Windows，进程是 LocalSystem 且在用户会话里）：

- Windows 把登录屏、锁屏、UAC 和 Ctrl+Alt+Del 屏放在同一个桌面（Winlogon）上，只有 LocalSystem 能打开；线程只能在自己附着的那个桌面上采集或注入。所以跟随就是：采集线程和注入输入的线程在桌面复制失效或要注入前，用 `OpenInputDesktop` + `SetThreadDesktop` 附着到当前接收输入的桌面（`platform/windows/desktop.cpp`，线程不能有窗口或钩子），再重建桌面复制。同一个 D3D 设备和编码器继续用，所以流公告不变，画面从下一帧继续。
- 桌面切换时 `AcquireNextFrame` 不止返回 `DXGI_ERROR_ACCESS_LOST`：离开非默认桌面时是 `DXGI_ERROR_INVALID_CALL`，切换中还会有暂时不可用的几种；`secure_desktop` 打开时它们都按“复制丢失”处理（暂停，由 250 ms 一次的重试跟着换桌面），关闭时 `INVALID_CALL` 仍然是采集失败。新桌面连续约 10 秒（40 次重试）都打不开，本次运行以 `RCS_ERROR_CAPTURE` 失败。
- 助手每 250 ms 看一次当前桌面（名称为 `Default` 是用户桌面，其他是安全桌面，包括 Windows 放屏保的 `Screen-saver`：它和 Winlogon 一样是另一个桌面，不跟随的进程既采集不到也注入不进，和“是否接受输入”的判断一致；打不开桌面（拒绝访问）也是安全桌面；会话里有没有用户用 `WTSQuerySessionInformation`），变化时发 `Events::desktopState`，会话发 `RCS_EVENT_DESKTOP`，并用 `DesktopState` 消息告诉观看端；观看端连上时得到当前的。
- 跟随时，输入桌面打得开就算接受输入，所以锁屏不再让授权暂停；输入的注入器带着 `secureDesktop`，注入前附着到输入桌面。
- 做不到时（不是 Windows、不是 LocalSystem、不在用户会话里）启动或助手连上时的检查（`session/checks.cpp`）以 `UNSUPPORTED` 失败，不会悄悄退回成暂停。

**Ctrl+Alt+Del**（`RCS_INPUT_SECURE_ATTENTION`）：观看端发，分享端先给 `on_input`，再由会话所在进程的 `testing::secureAttention`（生产里是 `platform::secureAttention`）执行；它不进注入器。`SendSAS` 只有会话 0 里的服务能调用，所以：会话在会话 0 的 LocalSystem 进程里时直接发（先读策略 `SoftwareSASGeneration`，不允许就返回 `PERMISSION_REQUIRED`）；会话在看护者启动的进程里时，经看护者交给它的管道一头（继承的句柄，管道只连着它，见下文看护者一节）请看护者发；都不是则 `UNSUPPORTED`。方案 B 里会话在服务里，所以输入经助手链接时，Ctrl+Alt+Del 由 `AgentInjector` 在会话一侧执行，不经助手。发到哪个会话：服务调用 `SendSAS` 时模拟哪个令牌，就发到那个令牌的会话（微软 `SendSAS` 文档的说明），所以会话 0 里的发送者先模拟目标进程的令牌（方案 B 是助手进程，方案 A 的中转是看护者持有句柄的那个被看护进程），再发；不模拟就是控制台会话。这样被看护进程在远程桌面会话里时，Ctrl+Alt+Del 也到它自己的会话。

**看护者**（组件 `components/supervisor`，单独的库 `RemoteControl::Supervisor`，只用核心的公开接口：日志经 `rcs_log_write`，失败原因经 `rcs_set_last_call_error`，Windows 的小工具和 `SendSAS` 是它自己的；被看护进程那一边留在核心的 `platform/windows/helper.cpp`）：一个线程（`src/supervisor_windows.cpp`）每 250 ms 看一次目标会话：`os_session` 为 0 时是 `WTSGetActiveConsoleSessionId`；否则用 `WTSEnumerateSessionsW` 找宿主选的那个会话，它的状态是 `WTSActive` / `WTSShadow` / `WTSConnected`（`rcs_os_session_state` 的 `ACTIVE`、`CONNECTED`）时才算“有桌面”（`src/sessions_windows.cpp`，`rcs_list_os_sessions` 也在那里）。设置（`os_session`、间隔、宽限）在一把锁下，`rcs_supervisor_update` 只改它们并把“宿主选过几次”加一，线程每一轮读一次，所以运行中、回调里都能改。两种账户都要求看护者是 LocalSystem（`SetTokenInformation(TokenSessionId)` 和 `WTSQueryUserToken` 都要 `SeTcbPrivilege`）。SYSTEM 方式用服务自己令牌的副本（`TokenSessionId` 改成目标会话），用户方式用 `WTSQueryUserToken`；`CreateProcessAsUser` 启动，`lpApplicationName` 明确给出命令行里的程序（`rcs_supervisor_create` 已经拒绝了不是完整路径、没有扩展名、带空格却没加引号的程序，所以系统从不按空格切分去猜 `C:\Program.exe`），桌面是 `winsta0\default`，只继承列出的句柄（`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`：停止事件，有 `helper_key` 时再加一个装着密钥的匿名管道的读端），属性列表、事件、环境块（`CreateEnvironmentBlock`）任何一步失败都让这次启动失败并说明原因，不会带着空的句柄列表或缺 `SystemRoot`、`PATH` 的环境启动。进程先挂起，放进它自己的 `KILL_ON_JOB_CLOSE` 作业对象（每个助手一个：一个作业只能装同一个会话的进程，所有助手共用一个的话，注销后在新会话里启动的助手放不进去），放不进去就结束它并报告 `HELPER_FAILED`（不能有比服务活得长的 SYSTEM 进程），再恢复运行。
- 给被看护进程的东西放在环境变量里，值的前面是看护者的进程号（`<pid>:<句柄>`）：被看护进程第一次读（`win::supervisorLink`）时用 Toolhelp 快照核对自己的父进程就是这个进程号，不是就当作没有（`UNSUPPORTED`，“不是 rcs_supervisor 启动的”），并且把这些变量从自己的环境里删掉、把句柄设成不可继承，所以它再启动的进程既看不到、也拿不到这些句柄。密钥只在 `rcs_helper_get_key` 第一次调用时从管道读一次，然后关闭管道。
- 重启的间隔：自己退出时等 `restart_delay_ms`，连续在 30 秒内退出才逐次翻倍（第一次就是 `restart_delay_ms`）；启动失败同样从 `restart_delay_ms` 起翻倍；没人登录（用户方式的 `ERROR_NO_TOKEN`）不算失败，只报告一次 `NO_SESSION`，有人登录后马上启动；控制台出现会话或换了会话时间隔清零。`WTSGetActiveConsoleSessionId` 在切换时短暂返回“没有会话”，被看护进程要连续 3 秒没有会话才被结束。线程意外出错时结束被看护进程、报告 `RCS_SUPERVISOR_FAILED`，状态里 `helper_running` 为 0，之后 `start` 可以重新开始；每次 `start` 都清零计数。
- 控制台换了会话则先置位停止事件、等 `stop_grace_ms`、再结束进程，然后在新会话里启动。
- 宿主选的会话（`os_session` 不为 0）：不再是 `ACTIVE` / `CONNECTED`（客户端断开、注销、被重置）时立即要求被看护进程结束，不等 3 秒（那 3 秒只是为控制台切换时的空档），并报告 `NO_SESSION`，原因写明是哪种状态；再次显示（客户端重连）时立即在里面重新启动，间隔清零。断开期间不留进程：断开的会话没有显示，采集和输入都做不到（`SendInput` 只在有客户端连着的会话里起作用），重连后由新进程重新打开显示器（见上文“远程桌面会话里的采集”）。会话从列表里消失后记为“已结束”，之后同号的新会话不会被当成它，直到宿主再次 `rcs_supervisor_update`（或重新 `start`）；SDK 从不改变会话的连接（例如 `tscon`），那是系统设置。
- SAS 中转管道每个被看护进程一条（安全评估 SA-08）：启动它之前看护者建管道（随机名、`FIRST_PIPE_INSTANCE`、只有一个实例，DACL 只有 SYSTEM 和管理员），自己打开客户端那一头并连上，把这一头作为继承句柄交给被看护进程（`RCSDK_SUPERVISOR_SAS` 里是句柄，不是名字），启动后关掉自己的副本。管道的唯一实例从一开始就被占着，别的进程（被看护进程同账户的也一样）既打不开名字、也占不住它，请求只能经看护者交出去的那一头来；发到哪个会话按看护者持有的被看护进程句柄的令牌（不按 `GetNamedPipeClientProcessId` 的进程号，进程号可能被复用）。被看护进程退出时它那一头关闭，中转随之结束；被看护进程记录（`Helper`）持有它，结束时先停中转。不是 `'S'` 的请求回 `INVALID_ARGUMENT`、什么都不发；所有等待都是重叠 I/O，并且和停止事件一起等，`stop` 总能返回。不用看护者生命期内一条、每次启动前把被看护进程的账户加进 DACL 的管道：那样同账户的任何进程都能连上，反复连接占住唯一的实例就让 Ctrl+Alt+Del 发不出去，而且只能按请求者的进程号发。

**保持唤醒**（模块 `modules/power`，`rcsdk.power`，不在核心里）：会话那一半在分享会话里，按设置和模块的 `connected` / `disconnected`（每个通过握手的连接，不管模块在它上面激活没有，所以观看端不用这个模块）算出现在要什么：有连接或 `while_running` 为 1 时是 `keep_awake`，否则 `OFF`；变了就经 `rcs_module_send_machine` 告诉机器那一半。机器那一半在助手里（本进程的 `MediaAgent`，或另一个进程里也注册了这个模块的助手），持有系统的电源请求（`request_windows.cpp` 的 `PowerCreateRequest`，`request_macos.mm` 的 `IOPMAssertion`），回答实际持有什么和结果。会话那一半据此发 `RCS_POWER_CHANGED`、答 `rcs_power_get_status`；新助手接上（`machine_joined`）时重新告诉它，助手离开时报告没有持有。系统拒绝只报告和写日志，不结束运行。测试经 `power_request.hpp` 的工厂换掉系统的请求。同一个模块还做网络唤醒的积木和远程电源操作（用法见[指南 15](guide/15-modules.md#电源rcsdkpower)）：分享端的问候告诉观看端它提供的操作和（宿主打开时）能唤醒机器的网卡；观看端的请求经分享端宿主的设置和回调，交给机器那一半检查、答复、延迟后执行（`system.hpp` 的 `Machine` 是系统的那一半，测试换成替身）；`rcs_power_send_wake` 在任何一端发魔术包。

**隐藏宿主自己的窗口**（`rcs_capture_config.exclude_current_process`）：默认关闭，宿主的窗口和其他窗口一样出现在画面里。宿主打开它之后：

- macOS 把整个应用从 ScreenCaptureKit 的内容过滤器中排除。
- Windows 的 `WDA_EXCLUDEFROMCAPTURE` 是按窗口设置的，所以每次桌面更新后，都会给本进程还没隐藏的顶层窗口补上，包括共享期间新开的通知、对话框和菜单。新窗口可能已经出现在刚取到的这一帧里，所以这一帧，以及 50 ms 内合成的画面都不交给编码器，观看端停在上一帧。宿主自己设成 `WDA_MONITOR` 的窗口保持不变；停止采集时，SDK 设置过的窗口恢复为 `WDA_NONE`。
- 可见的窗口隐藏不了时，本次运行以 `RCS_ERROR_CAPTURE` 失败并说明原因，不会照常共享出去。Windows 10 2004 之前的系统没有 `WDA_EXCLUDEFROMCAPTURE`，这些窗口在画面中显示为黑块。

**路径和迁移**（`protocol/udp_connection`，用法见[指南 09](guide/09-transports.md#网络变化连接迁移)）：UDP 连接由密钥和连接编号标识，不由地址标识。每个数据报 `0xD8 cid(4) 包号(8) 密文`，cid 是收方的编号（握手的导出密钥派生，两端各一个）；分享端的 UDP 端口（`UdpDemux`）只按 cid 分路，不认识的 cid 回 `0xD7 cid`（“不认识这个连接”，和版本通知共用预算）。连接有一组路由（本端的套接字加 `Link`：直连、ICE、中继、宿主通道）和每条路由上对端的候选地址；流量只走当前的那个。对端从新地址来时，只有认证过、包号最新、不是探测（只含 `PATH_CHALLENGE` / `PATH_RESPONSE`）的数据报才触发：新地址先验证（挑战带 8 字节随机数，验证前往那里发的不超过收到的 3 倍，填充只在限额内），同时挑战旧地址（RFC 9000 9.3.3），答上了才换。路径状态：运行中 → `path_stall_ms` 没声音是停滞（视频、声音、数据熔断，只发 PING、挑战和控制流）→ `idle_timeout_ms` 没声音是没了（留着：观看端到重连时间窗、分享端 `path_hold_ms`）；迁移关着（PING / PONG 的一位，任一端关就都关）时没了就结束连接。TCP 上每条 TCP 连接是一条路径：分享端的 `TcpDemux` 把数据交给连接编号是哪个连接的，不管它从哪条 TCP 连接来；某条 TCP 连接关了，归它的连接在它上面来的数据之后收到 `transport::PeerClosed`，那正是当前路径时立即停滞（`PEER`），迁移关着时连接结束。观看端的 `TcpRedial`（`session/connect`，和 `PathRestart` 同一个接口 `PathFinder`）在路径停滞（1 秒宽限）、网络变了而路径不回答、TCP 连接断了、网络选择换了网络、要备用路径时，在自己的线程上向分享端再连一条 TCP（绑定选定的网卡），经 `addRoute` 交给连接；路径回来之前每秒一条。TCP 的路由是有序的（`Link::ordered`）：探测不填充，可靠消息和重复的补要至少 1 秒才重发，一帧剩下的分片要等后一帧的分片到了或 1 秒没有东西才要，一帧至少等 2 秒才放弃，自动 FEC 不加冗余。本端的网络变了（`transport::networkGeneration`，平台的监视器推动，`platform::watchNetworks` 在运行期间拿着）或发送失败时当前路径要回答挑战，不回答就停滞，直连 UDP 的观看端换一个新套接字（`UdpOptions::reopen`），中继有移动票据时把分配挪到新套接字。换了路径：可靠流马上重发未确认的，往返重新估计，码率 `newPath`（只换端口时不），换路径之前开始的 `REPORT`（带 `first`）和发往别的路径的探测包不算丢包。P2P 和中继的新路径由 `session/connect` 的 `PathRestart` 找：一个自己的线程，`ice::Agent` 重新收集、经 `on_signal` 发出用连接密钥封好的重启信令、等对端的回答、连通检查，选中的套接字经 `UdpConnection::addRoute` 交给网络线程（它验证后再换过去）；两端同时发起时观看端的继续。`NetworkChoice`（`session/path_info`）按 `rcs_paths_config` 和 `local_interface` 决定本端的套接字绑定到哪块网络的网卡（或不绑定），变了时连接换过去（`moveRoute`、`PathRestart::move`），一块都不剩时 `leaveRoute`；备用路径（`standby`）是另一块网络上的一条路由，每 `standby_probe_ms` 证明一次，停滞时立即接手，原来的路径答上了再换回来。

**连接断开**（`Session::run` 的循环）：一次运行里，连接结束的异常回到会话层。对端关闭连接是 `RCS_ERROR_DISCONNECTED`（连接的关闭帧），路径断了是 `RCS_ERROR_NETWORK`（迁移关着时 `idle_timeout_ms` 没收到数据、TCP 连接断了；迁移的连接是路径没了以后留着的时间用完，或对端说已经没有这个连接（`UnknownConnection`），这时观看端不等重连间隔）。分享端在观看端连上过之后遇到这两种，按设置回到 `LISTENING` 再跑一次角色代码；观看端在运行过之后遇到 `NETWORK`，进入 `RECONNECTING`，按间隔重跑角色代码直到连上或时间窗用完（从路径断开算），期间 `CONNECTING` 不再报告。路径没了而连接留着时观看端也报告 `RECONNECTING`，路径回来后回到 `RUNNING`，还是同一个连接（`Session::pathLost` / `pathBack`）。每次重跑都是一次完整的连接：握手、协商、解码器、声音、授权，所以没有“恢复会话”的协议状态要同步，观看端只需从关键帧继续。连接之间释放按住的键、清空输入队列和对端 / 流信息；其他失败照旧结束运行。

**停止通知**：`Session::stop(reason)` 在取消之前记下原因，运行结束时连接上的最后一条消息就是它（`LeaveNotice` 在连接销毁前把原因交给 `Connection::leaving`）：关闭帧里的一个原因字节（停止或重启）。收到的一端把它变成 `RCS_ERROR_DISCONNECTED`（停止）或 `RCS_ERROR_RESTARTING`（重启）；观看端对 `NETWORK` 和 `RESTARTING` 重连，分享端对三种都回到监听。连接关闭时先发出还没发过的消息，再发两遍关闭帧（路径没了时不发：路径回来以后才到的关闭帧是过时的）；TCP 上观看端的 `FramedSocket` 关闭时，最近一秒写过东西的连接先 shutdown 再读完对端的数据（最多 250 ms），分享端的 `TcpDemux` 也是写完最后的话、shutdown、等对端关，避免复位吞掉通知。进程崩溃来不及通知，对端靠无数据的超时（默认 5 秒）发现，报告 `NETWORK`。

**数据通道**（`session/data_channel`）：宿主的消息和每个模块的消息各进一个队列（宿主的上限是 `rcs_network_tuning_config.send_queue_bytes`，模块的是它的 `max_message_bytes`），每个连接有自己的发送线程轮流从各个队列取一条；模块清单和问候排在所有消息前面。可靠消息切成 `DataPart`（UDP 1 KiB，TCP 64 KiB）按顺序发；UDP 上交给连接、还没确认的字节数超过 `in_flight_bytes`（`RCS_EXT_NETWORK_TUNING`）就等确认（条件变量，确认到达即醒）。UDP 上它们走自己的有序流，输入和其他控制消息不排在它们后面；发送线程先发视频，有视频分片待发时数据不发；数据还有自己的拥塞窗口，限制在路上的数据：视频在发时按观看端的报告调整——路径通畅时每个报告放大四分之一，一出现排队（10 ms）或丢包（2%）就减半，比视频码率的反应（30 ms、10%）早得多，所以数据只用视频剩下的带宽，不压低视频的节流和码率；画面静止时（或者在观看端，它不收报告）由确认放开，只受 `in_flight_bytes` 限制；画面恢复时窗口从小重新开始。不可靠消息是 `DataDatagram`，UDP 上像声音一样立即发、不重传。`DataPart` 的 `kind` 说明它是什么（宿主的数据、模块消息、模块清单、模块问候），模块的带着它在发送方清单里的编号。接收端在读取线程上拼回整条，宿主的交给 `on_data`，模块的交给会话的 `ModuleSet`。换路径时队列不动，在路上的照常确认和重发；连接结束时队列清空。

**模块**（`modules/`、`session/modules`，见 [MODULES](MODULES.md)）：注册表（`modules/registry`）记着进程里注册的模块。每个会话有一个 `ModuleSet`：创建时为配置里的每个模块建一个会话那一半的句柄（`rcs_module_impl`），每次运行开始时 `start`、结束时 `stop`。每个连接通过握手时，`ModuleSet::peerConnected` 先告诉每个运行中的模块（`connected`，不管它在这个连接上会不会激活），连接结束时在 `peer_left` 之后告诉 `disconnected`（`peerGone`），所以只关心“谁连着”的模块（保持唤醒）不需要对端也用它。每个连接上观看端先发模块清单，分享端回自己的，两端据此决定哪些模块在这个连接上激活（`peer_joined`、`peer_left`），之后按模块编号分发消息和问候。对 SDK 的协议的违反（编号不在对方清单里、比 `max_message_bytes` 大）和其他协议错误一样让连接失败；模块自己的消息格式由模块检查。进入一个模块的调用由它的递归锁串起来（`modules::call`），模块在调用里发的事件等调用返回才交给 `on_event`（`afterCall`），宿主在 `on_event` 里等模块时就不会互相等。机器那一半在 `MachineHost` 里跑（一个线程，按顺序做 start、消息、stop）：分享端在助手里（`MediaAgent` 带一个；另一个进程的助手经链接收 `ModuleStart`、`ModuleStop`、`ModuleData`，回 `ModuleStatus`、`ModuleData`，新的助手接上时 `RemoteAgent` 把要过的模块再要一遍），观看端在 `ModuleSet` 自己的 `MachineHost` 里。剪贴板、文件传输、保持唤醒、缩略图、文本插入点是 `modules/` 里的官方模块（`clipboard`、`file_transfer`、`power`、`thumbnail`、`text_caret`），只用公开头文件。控制状态变化时 `ModuleSet::peerControl` 告诉在那个连接上激活的模块（`peer_control`）：分享端由 `Session::viewerControlChanged`，观看端由收到的 `ControlGrant`；模块激活时正有控制就紧跟着 `peer_joined` 告诉一次。机器那一半的 `rcs_module_get_shared` 经 `MachineHost::Output::shared` 问它所在的 `MediaAgent`。

**观看端**（每路一个解码线程、网络读取线程、发送线程；运行线程等着它们中第一个失败）：

```text
读取线程：Connection.receive ──▶ 按路的 ReceiveQueue ──▶ 这一路的解码线程：（宿主 on_video）──▶ VideoDecoder.decode ──▶ 交付回调
发送线程：InputQueue ──▶ Connection.send（输入、按路的关键帧请求和丢失报告、心跳）
```

- 第 0 路的解码器在开始时就打开（Windows 上要几百毫秒，和分享端开始采集同时进行），别的路在第一个关键帧到时打开，一路结束（`StreamEnded`）就关掉它的解码器。

- 宿主要编码包时（`RCS_EXT_ENCODED_OUTPUT`），解码线程把每个从 `ReceiveQueue` 取出的包先交给 `on_video`，再解码，所以宿主拿到的顺序就是解码的顺序，慢的回调拖住的是画面，不是连接的读取线程。`ReceiveQueue` 丢弃积压之后、或者传输放弃一帧（`StreamGap`）之后重新开始的那个关键帧（`Item::resetDecoder`）交出时带 `after_gap = 1`。`decode` 为 0 时不开解码器：包只交给宿主，解码器设置的变化也不重开什么。

- 关键帧的编码格式或尺寸和当前解码器不同时（分享端切换了画质），换一个新解码器再解码。
- `ReceiveQueue` 有最大排队时间（`RCS_EXT_VIDEO_TUNING` 的 `max_queue_age_ms`，默认 80 ms）和容量上限（`queue_frames`，默认 64 帧，只用于限制内存：路径停顿后一次到达的一串帧照常解码）。超出时整段丢弃积压的包，一直丢到下一个关键帧，同时请求关键帧，并在该关键帧处重置解码器。它从不单独丢弃某个参考帧之后继续解码。
- 限制以时间为准：帧数上限在不同帧率下代表的时间不同（3 帧在 60 FPS 是 50 ms，在 120 FPS 只有 25 ms）。解码器偶尔卡一下（大关键帧）时，追上积压比用新关键帧恢复便宜得多。
- 重置解码器不会销毁解码会话：不重排序时解码器内没有缓存帧，下一个关键帧本身就重新开始预测。只有参数集变化时才重建会话（VideoToolbox 重建一次约 45 ms）。
- 解码器出错时（例如休眠唤醒或 GPU 重置让解码会话失效），观看端换一个新解码器，丢弃积压并请求关键帧，从下一个关键帧继续，计入视频接收统计的 `decoder_recoveries`，并发出 `RCS_EVENT_RECOVERY`。恢复后还没解出一帧就再次出错，说明不是偶发问题，本次运行以解码器的错误结束。
- `InputQueue` 合并连续的鼠标移动；队列满时整体替换为一个“释放全部按键”事件，并返回 `QUEUE_FULL`。

**声音**（`session/audio_stream`，用法见[指南 12](guide/12-audio.md)）：

- 声音有两个方向：分享端的机器（或宿主）到每个观看端，观看端的麦克风（或宿主）到分享端。每个连接上两端各有一个 `SoundOut`（发）和一个 `SoundIn`（收），一个方向在发送方发、接收方收、又有共同格式时才传。`CodecOffer` 带每端两个方向各自的格式列表和“现在发不发、收不收”；声音消息不带方向，由连接的哪一端发来决定。任一端的设置变了（`enabled`、格式、码率、观看端的请求……），发送方重新开始编码，发新的 `AudioFormat`，接收方按新格式重开解码和播放；停止时发 `AudioOff`，带上原因（发送方关了、接收方关了、没有共同格式）。
- 一端只采集一份（`AudioHub`），按每个连接协商的格式分别编码：分享端的机器声音和麦克风在助手里采集（助手的 `AudioStart` 说明采哪个），观看端的麦克风在会话的进程里采集，宿主推送的声音直接交付。分享端收到的每个观看端的声音各开一路播放（系统混音），或分别交给宿主的 `on_audio`（带 `viewer_id`）；观看端之间不互相转发。
- 回环：分享端发机器的声音、又由 SDK 播放观看端的声音时，播放在会话的进程里，所以必须排除本应用的声音、助手也在本进程（`session/checks.cpp` 的 `checkSendAudio`），否则 `UNSUPPORTED`；观看端不能发机器的声音（那里播的正是分享端的声音）。麦克风一侧的回声靠系统的回声消除（macOS 和 iOS 是 `AVAudioEngine` 的语音处理，Windows 是通信类别端点上驱动自带的，Android 是 AAudio 的 `VOICE_COMMUNICATION` 预设），是否在运行由事件报告。

```text
发送方：AudioCapture / 麦克风 / 宿主推送（平台线程）──▶ AudioHub ──▶ 每个连接的 AudioSender：凑满一包 ──▶ audio::Encoder ──▶ Connection.send(AudioPacket)
接收方：读取线程 ──▶ SoundIn 的声音线程：按序号补丢失的包 ──▶（宿主的编码包 on_audio）──▶ audio::Decoder ──▶ Playout ──▶ AudioPlayer（设备的实时线程）
                                                                                                         └──▶ 宿主 on_audio
```

- 采集和播放各有一个管理线程：设备打不开、被拔掉、默认设备变了，就关掉重开，每 2 秒重试一次，并通过 `RCS_EVENT_AUDIO` 报告。声音的问题从不结束本次运行。
- 编码的声音包（`RCS_EXT_ENCODED_OUTPUT` 的 `on_audio`，`direction` 说明是哪个方向）：发出的包在 `AudioSender` 编码之后、发出之前交，分享端每个观看端的发送线程交自己的（`viewer_id`）；收到的包在 `AudioReceiver` 里、解码之前交，和解码器拿到同样的包（比后来的包晚到的丢掉）。`decode` 为 0 时观看端不建解码器和播放设备，只交包，并照样统计收到和丢失的包数。
- UDP 上声音是单独的帧类型：发送线程每轮先发声音，再按节流发视频，声音不重传。数据报的连接（`Connection::lossy`）上 `AudioSender` 默认把上一个编码好的包再附在这个包后面（`redundancy`，两者合起来放得进 1024 字节时；PCM 不附）：接收端缺了正好一个包、而下一个包带着它时，先解附带的那份、再解这个包，照原样补回（`recovered_packets`）；更长的空缺由解码器补出前面缺的部分（Opus 延续声音，其余补静音），超过 50 包的空缺不补，交给播放缓冲重新预充。
- `Playout` 是无锁的单生产者单消费者环形缓冲：
  - 预充目标：设备一次取数的量 + 一个包长 + 最近 8 秒到达时间的波动幅度（16 个半秒的桶；Wi-Fi 省电的打盹每几秒一次，2 秒的窗口在两次之间缩小，下一次就放空；欠载一次后测到的最大波动再保持 30 秒播放时长，同样大的波动再出现就续上，保持期内或刚过不久又欠载就把保持时长翻倍（最多 10 分钟）；开头 3 秒至少按 40 ms 算）+ 余量（`buffer_margin_us`，默认 2 ms），最少 1 ms，最多 `max_delay_ms`（默认 200 ms）；设备一次取数加一个包就超过 `max_delay_ms` 时，上限是这两者之和。
  - 时钟漂移的补偿：每半秒的取数看一次这期间取数时缓冲的最低水位。低于“一次取数 + 余量 + 半个抖动”就交叉淡化地重复一段（每次最多 5 ms）；高出这条线一个区间（抖动、余量和 1 ms 中最大者的两倍）就交叉淡化地丢掉一段。两种调整都回到区间中间。局域网上缓冲因此只比一次取数多几毫秒。
  - 开始播放时、积压比目标多 20 ms 以上（例如网络卡顿后一起到达）或超过上限时，直接跳回目标。
  - 取数从不读还没到的声音：缺的部分第一次用刚播过的声音交叉淡化补上，连续两次就补静音并重新预充。
- 声音包的长短跟着采集走：Windows 每 10 ms、macOS 默认的 Core Audio 进程捕获每 5 ms（240 帧，可改）、ScreenCaptureKit 每 20 ms 交一次声音，Opus 就编成同样长的一个包（最长 20 ms）。比采集块更小的包不会更早发出，只会变成连发（实测连发的小包在局域网上有约 0.1% 丢失）。AAC 固定 1024 帧，PCM 固定 5 ms。每个包最多 1024 字节，装进一个数据报：Opus 编码器以此为上限（20 ms 一包时码率实际最多约 400 kbit/s），AAC 用恒定码率（macOS 320 kbit/s 时最大的包约 970 字节，超出上限时编码器报错），PCM 每包 960 字节。
- 采集端的声音流不中断：Core Audio 捕获不跟随被捕获的声音启动（`TapAutoStart` 关），设备一直运转，静音时也交付；Windows 普通环回静音时没有数据，SDK 每 10 ms 补一段静音。这和 Sunshine 的“continuous audio”一样，观看端缓冲始终就绪。
- 平台采集：macOS 默认用 Core Audio 进程捕获（`platform/macos/audio_tap.mm`）。在私有聚合设备上挂一个全局立体声捕获，需要时排除本应用：本进程和它启动的子进程（Chromium、Electron 在辅助进程里放声音；launchd 替它启动的 XPC 服务不是它的子进程，排除不了），进程列表变化时更新捕获的排除名单。聚合设备的输入里，输出设备自己的输入（例如耳机的麦克风）排在捕获之前，捕获的位置按输出设备的输入流推算，每次回调都核对。实时回调只把数据写进无锁环形缓冲并发信号，由 SDK 自己的线程取出、必要时用 AudioConverter 转成 48 kHz，再交付。默认输出设备变化时重建这个捕获。ScreenCaptureKit 仍可选，它只排除本进程自己的声音。Windows 用 WASAPI 环回或进程环回（排除整个进程树）。
- 平台播放：
  - Windows 共享模式：默认在设备队列里放一个半混音周期（`playback_queue_frames` 可改）。宿主要求更短的周期、且设备的混音格式是 48 kHz 立体声浮点时，用低延迟共享模式。
  - Mac：输出每周期 256 帧（5.3 ms，可改）。默认输出设备变化时重新打开，周期也用到新设备上。
  - Windows 的采集和播放线程都登记为 MMCSS“Pro Audio”。
  - iOS：整个应用一个 AVAudioEngine（`platform/ios/audio_engine.mm`），所有会话的播放和麦克风都在上面：播放是混音器上的 `AVAudioSourceNode`（实时线程从播放缓冲取声音），麦克风是输入节点上的一个 tap（转成 48 kHz 立体声交给每个麦克风），回声消除是输入节点的语音处理，它只消掉经同一个 I/O 单元播出的声音，所以 SDK 播放的对方的声音会被消掉。引擎有自己的线程：部分加入或离开、设置改变、系统打断、线路或格式改变（`AVAudioEngineConfigurationChangeNotification`）、媒体服务重置时，按当时有哪些部分重新搭（拆掉、设声音会话、接上节点、开始），失败时每 2 秒再试，并把每个部分的状态（在跑、原因、语音处理）告诉它。部分要的东西（周期、麦克风的设备和语音处理）在加入时读、持锁时抄进计划，之后只用抄下的；移除一个播放器会清掉它的槽并等实时线程用完，移除麦克风会等正在进行的交付结束。
  - Android：AAudio，播放和每个麦克风各是一个流（`platform/android/audio.cpp`），各自一个线程看着：打开失败时每秒再试、原因只报告一次，设备没了（错误回调）就关掉重开。麦克风是单声道输入，交付时复制成立体声；回声消除靠 `VOICE_COMMUNICATION` 预设（系统做）。录别的应用的声音只有 Java 的 AudioPlaybackCapture，由宿主推。

## 暂停和应用的前后台

暂停有三个来源，各记各的：宿主的（`rcs_session_pause` / `resume`）、应用进后台的（`RCS_EXT_BACKGROUND`），和观看端从分享端听到的（分享端暂停了，`pausedByPeer_`）。`Session::pauseChanged` 把它们合起来报告 `RCS_EVENT_PAUSE`（`by_host`、`by_background`、`by_peer`）；本端自己的两个（`paused()`）变了就让 `settingsVersion` 往前走一步（没在运行时只记下，运行开始时有暂停着的先报告一次）。对方的暂停在连接断开时清掉。

- **应用的前后台**：`seams::watchAppState` 是一个进程级的转发点。第一个观看会话创建时它请平台开始观察（`platform::observeAppState`：iOS 用 UIKit 的进后台、回到活动的通知，其他系统从不调用），之后把每次变化告诉所有登记的会话；登记的句柄销毁时等正在进行的那次调用结束，所以会话在析构的第一步就交还它。测试用 `seams::setAppBackground` 代替系统。
- **观看端**：发送线程看到 `settingsVersion` 变了，就把暂停位放进观看端设置（`ViewerSettings.paused`）发给分享端，并按暂停的声音关掉或重新打开接收的声音；解码线程每轮看 `paused()`：暂停画面时关掉解码器、到的包直接丢掉，恢复时如果分享端还没结束这路流（它比本端的暂停先恢复了），就从关键帧重新开始。
- **分享端**：观看端暂停画面时 `Viewer::takes()` 对每一路都是否，于是和“观看端不要这几路”走同一条路：给它发流结束，没有观看端要的路停掉采集和编码；恢复后重新公告、从关键帧开始。暂停声音时不再给它发声音（`SoundOut::apply`）。
- **分享端自己暂停**：暂停画面时视频计划里每一路都没人要（`taken = 0`），同样结束每个观看端的流、停掉采集和编码；暂停声音时 `takesSound` 对每个观看端都是否。每个观看端记着最后告诉它的暂停位（`pausedTold`），变了就发 `SharerPaused`，观看端的视图线程收到后交给 `Session::peerPaused`。应用前后台的观察对两种角色都登记，iOS 分享端进后台时默认暂停画面。

## 帧交付

宿主只能注册 `on_gpu_frame` 和 `on_frame` 其中一个（两个都注册时，创建会话返回 `INVALID_ARGUMENT`），SDK 不会在两者之间自动分流。

- **`on_gpu_frame`**：原生 GPU 表面。macOS 和 iOS 是 `CVPixelBuffer`，Windows 是 `ID3D11Texture2D` 及其设备，Android 是 `AHardwareBuffer`。解码帧的布局跟着画面格式（NV12、P010、NV24、P410，`rcs_gpu_frame_info.format`），颜色在 `color` 里，由宿主在 GPU 上转换成 RGB，几乎不花时间。Windows 的软件解码在 CPU 上解出、转成 BGRA，再上传成 BGRA8 纹理交出，所以 `software_decoding_gpu_frames` 在四个系统上都是 1（macOS、iOS 和 Android 的软件解码器本来就输出 GPU 表面）。分享端的本地预览帧：macOS 是采集给的 YCbCr 布局，Windows 是 BGRA。
- **`on_frame`**：CPU BGRA 图像，这是宿主明确要求的。帧在 GPU 上时由 SDK 读回内存。macOS 上解码器直接输出 BGRA，读取时零复制映射；转换由 VideoToolbox 完成，比 CPU 转换快。Windows 硬件解码时由 GPU 的视频处理器转成 BGRA 再读回。需要在 CPU 上转换 NV12 时（例如 Windows 软件解码），ARM 用 NEON、x86-64 用 SSE2。
- 分享端不需要预览时不要注册帧回调，因为回调在采集编码循环里同步执行。
- GPU 帧带引用计数，宿主 retain 之后，即使会话已停止也仍然有效。Windows 解码帧会复制出解码器的内部池，避免占住解码器的表面。

## 硬件与软件

`rcs_acceleration` 由宿主选，SDK 不会自己在硬件和软件之间切换：

- `HARDWARE`（默认）：只用 GPU / 媒体引擎的编解码器，没有就报错。
- `SOFTWARE`：只用 CPU 编解码器，例如给没有可用 GPU 的机器。
- `HARDWARE_FIRST`（只用于解码器）：每路流开始时按它的编码格式和画面格式选，硬件解码器能解就用硬件，不能就用软件。观看端的报价是两者能解的合在一起，所以 Windows 观看端能收 HEVC 4:4:4（只有软件解码器解），4:2:0 仍在 GPU 上解。选了哪一个由 `RCS_EVENT_CODEC` 报告；解码器失败时不会换成另一个。

`rcs_list_codec_implementations` 列出本机每一个编解码实现（格式、方向、硬件或软件、后端、所在 GPU、名称）；`rcs_get_capabilities` 是它的摘要，另外报告软件解码能否产生 GPU 帧。结果在进程内只探测一次。Windows 上同一块 GPU 可能被 DXGI 列成多个适配器（例如装了虚拟显示驱动时），列表按物理 GPU 去重，并按厂商过滤编码器。

| | macOS 硬件 | macOS 软件 | Windows 硬件 | Windows 软件 |
| --- | --- | --- | --- | --- |
| H.264 编码 | VideoToolbox 硬件编码器 | VideoToolbox 软件编码器（逐帧完成，见下） | 采集显卡的硬件编码器 | Microsoft 软件编码器 |
| HEVC 编码 | VideoToolbox 硬件编码器 | 不支持（1080p 每帧约 87 ms，达不到实时） | 采集显卡的硬件编码器 | 不支持（软件 HEVC 编码器达不到实时） |
| H.264 解码 | VideoToolbox 硬件解码，GPU 帧 | VideoToolbox 软件解码，仍是 GPU 帧 | 系统解码器 + D3D11，GPU 帧 | 系统解码器，CPU 帧 |
| HEVC 解码 | VideoToolbox 硬件解码 | VideoToolbox 软件解码 | 需要“HEVC 视频扩展”和 D3D11 HEVC 解码能力 | 需要“HEVC 视频扩展”，CPU 帧 |
| AV1 编码 | 不支持（Mac 没有 AV1 编码器） | 不支持 | 采集显卡的硬件编码器（如 RTX 40 系） | 不支持 |
| AV1 解码 | VideoToolbox 硬件解码（M3 起） | 不提供 | 需要“AV1 视频扩展”和 D3D11 AV1 解码能力 | 需要“AV1 视频扩展”，CPU 帧 |

iOS 和 macOS 一样用 VideoToolbox（模拟器里没有硬件编解码器）。Android 用 MediaCodec（`RCS_BACKEND_MEDIACODEC`）：硬件是芯片厂商的编解码器，软件是系统自带的（`c2.android.*`、`OMX.google.*`：H.264 编解码，HEVC、AV1 只解码）。各系统的对照见开发者指南的[各平台的差别](guide/19-platforms.md)。

AV1 的码流约定：`data` 是一帧的 OBU（低开销格式，每个 OBU 带长度字段），不含时间单元分隔符；`config` 是序列头 OBU。Windows 解码时每帧前补一个时间单元分隔符，关键帧再带上序列头；macOS 把序列头解析成 `av1C`（AV1CodecConfigurationRecord）放进格式描述。关键帧标志取自编码器对输出样本的标记（`MFSampleExtension_CleanPoint`），不靠解析帧头。

能力报告如实反映本机情况。例如没装 HEVC 视频扩展的 Windows 不报告 HEVC 解码，这时只列了 HEVC 的观看端 `start` 会返回 `UNSUPPORTED`；宿主如果接受 H.264，就把它也列进 `rcs_decoder_config.codecs`。默认配置只用 H.264，几乎所有 GPU 都能硬件编解码。

软件编解码路径已在两台实机上通过编解码往返测试，但还没有做端到端的性能测量。VideoToolbox 的软件编码器即使设置了实时模式和 `MaxFrameDelayCount = 0`，也会攒约 13 帧做前向分析；SDK 每提交一帧就让它完成，所以每帧都立即输出（M4 Pro 上 1080p H.264 约 8 ms）。

## 安全

- 身份（核心配置的 `identity` 和 `rcs_device_identity_config`，`transport/credentials.hpp`）：两端共有的秘密是 32 字节随机密钥（默认，由宿主生成并通过自己的业务通道交换）、口令（PBKDF2-SHA256 60 万轮派生，盐由分享端每次运行随机选，在握手里告诉观看端）、短 PIN（CPace）或者没有；另外每端可以有一个长期设备密钥（P-256），以及它信任的对端公钥列表。
- 握手（`transport/handshake.hpp`）：双方各生成一次性的 P-256 密钥对（macOS 用 Security.framework，Windows 用 CNG，Android 用 BearSSL），交换随机数和公钥。会话密钥由 HKDF 派生，输入是 ECDH 结果，盐是共享秘密派生的密钥，信息里包含两条问候消息。有设备密钥的一端用它对“角色标签 + 之前的全部握手内容”签名，和公钥一起放进身份块，用握手派生的身份密钥加密（被动监听看不到设备公钥）；对端验证签名，并核对公钥在不在信任列表里。双方再各自发一个“finished”校验值（HMAC），证明自己持有共享秘密、看到的是同一份握手内容。所以：
  - 没有共享秘密、或没有受信任设备私钥的人无法完成握手，也无法在中间替换公钥；
  - 会话结束后一次性私钥被丢弃，即使共享秘密或设备私钥日后泄露，录下的流量也解不开（前向保密）；
  - 口令不是 PAKE：录下一次握手、或者自己连上分享端的人，都能拿到可以离线验证口令的值，每猜一次的代价是 60 万轮 PBKDF2，所以口令必须是长随机串。
  - PIN 是 PAKE（`crypto/cpace.hpp`，CPACE-X25519-SHA512，用 `third_party/monocypher` 的 X25519、Elligator 2 和 SHA-512）：两条问候消息各带一个 32 字节的 CPace 份额（客户端的随机数当 sid，信道标识是协议魔数和两个角色名），两端算出的 ISK 前 32 字节代替连接密钥作为 HKDF 的盐，此后和连接密钥的握手完全一样。份额之外没有任何从 PIN 派生的东西上线，录下的握手无从验证猜测；每次握手对方能验证一次猜测（finished 校验）。客户端每个问候只验证一个答复（`ClientHandshake::spent`）：冒充分享端的人（路径上的、伪造分享端地址的）每次连接只能猜一次，猜错观看端就以 `AUTHENTICATION` 结束运行。UDP 上分享端只回答证明了地址（Retry 的 cookie）的 PIN 问候。分享端从 PIN 问候起为每个握手占一次尝试（`PinAttempts`），回答了却没有得到客户端的证明就结束算一次失败，回答之前就拒绝的（低阶份额）不算；失败加上进行中的达到 `pin_attempts` 时，PIN 问候只得到一条未认证的短通知（`RCSPIN` + 版本，UDP 上是数据报 0xD6），宿主重新设置身份后重新计数。
  - 信任核对在签名验证之后：分享端不信任观看端的公钥时，已经和它协商出了会话密钥，就用这些密钥发一条加密的拒绝消息（`Refused`）再断开，观看端据此报告“被拒绝”而不是断网。
- 记录与数据报：AES-256-CTR（基于系统的 AES 分组运算）加密后，再用 HMAC-SHA256 截断到 128 位认证序号、头部和密文（先加密后认证）。两个方向使用不同密钥；每个序号只用一次，所以密钥流不会重复。
  - 助手链接的记录（`SecureChannel`）：序号严格递增，重放、乱序或篡改的记录都会被拒绝。每条记录最多 64 MiB，大于任何编码帧可能的大小：编码器的输出最多和未压缩的画面差不多大（原始的 I_PCM 宏块加上头约多 1%），流的最大画面（35 651 584 像素，8192×4352）未压缩的 NV12 是 53.5 MB；500 Mbit/s、1 FPS 时一帧的全部预算 62.5 MB 也放得下。宿主自己的格式的包按同一个上限检查（`protocol::maxVideoBytes`）。64 MiB 也是 UDP 上一条消息按最小的数据报切片时 16 位片号放得下的最大值。发送端也检查这个上限。
  - 网络上的数据报（UDP、TCP、P2P、中继、HOST、转发器）：每个数据报带 64 位包号，1024 个包的窗口拒绝重放；伪造或损坏的数据报当作丢失处理。认证过、内容却不合格式的帧只可能来自对端，结束连接；帧号窗口、一帧的分片数和等待补齐的视频总量都有上限，对端发不出占满内存的帧。
- 握手失败的连接只影响它自己，分享端继续监听（见“连接与协商”）。
- macOS 的无人值守没有看护者：LaunchDaemon（root）里的会话不碰屏幕，LaunchAgent 里的助手采集；会话按系统报告的对方进程和账户（`RCS_EXT_AGENT_LINK` 的 `on_agent_attach`）决定接受哪个助手，所以链接密钥可以让各个用户的助手读到。桌面状态来自助手所在窗口服务器会话的 `CGSessionCopyCurrentDictionary`（`platform/macos/window_server.mm`）。
- 无人值守时的权限：看护者在 LocalSystem 的服务里运行，它启动的助手（或方案 A 里的整个会话）也是 SYSTEM，网络端口、握手和所有解析都在 SYSTEM 里，所以网络握手在任何其他解析之前完成（一直如此），能用用户身份的助手就别用 SYSTEM。看护者的命令行指向的程序以看护者的权限运行，宿主要让它只有管理员能改。`secure_desktop` 让远端看到并操作登录屏，所以 `rcs_capture_config` 里默认关，文档要求宿主只在信任观看端时打开。Ctrl+Alt+Del 的中转管道每个被看护进程一条、启动前就连着它继承的那一头，别的进程打不开、占不住，请求只有一个字节；SDK 只读系统策略，从不修改。被看护进程的环境变量只在父进程是那个看护者时才被采用，密钥经继承的管道交给它，不出现在命令行或环境里。SDK 不提供任何隐藏运行、规避检测的东西。
- 会话和助手之间的链接（`rcs_agent_link_config`）：本机端点由系统的访问控制把关（Windows 命名管道的 DACL：会话所在的用户、SYSTEM、管理员，`ANY_USER` 时再加上已认证用户的读写权限，但不给创建管道实例的权限，别的用户不能冒充会话一侧，会话活着时总留着一个实例，名字不会空出来；Unix 套接字所在的目录先打开并核对：属于本账户或 root，别的账户改不了其中的条目（组和其他人不可写，除非带粘滞位；Android 上应用自己的组可写），否则以原因失败；之后的操作都相对这个目录的描述符。套接字文件在 `bind` 之后、`listen` 之前核对它确是本账户的套接字，再用 `fchmodat(…, AT_SYMLINK_NOFOLLOW)` 设成 0600 或 0666，不经路径跟随链接，这就是实际的权限，不受进程 umask 影响，也不改动别的线程看到的 umask；旁边的 `<端点>.lock` 由监听者锁着（必须是本账户的、没有别的名字的普通文件），用来分辨死掉的进程留下的套接字文件和队列满了的活监听者；名字被别的进程占着时原因说明是哪个账户（Windows：哪个进程）），只接受本机客户端。之上依次是：系统报告的对方进程、账户和程序（macOS 取自连接的审计令牌，助手说完握手后再核对一次，期间把连接交给别的进程或 exec 成别的程序的被拒绝；会话交给宿主的 `on_agent_attach` 决定，助手按 `access` 决定要不要理会话）、明文的版本前缀（链接格式和 `RCSDK_VERSION`）、绑定到链接用途的密钥派生出的安全通道。链接上的每条消息解码时逐字段校验（结果码只能是本版本定义的值，失败不能是 `RCS_OK`，内容区域不能超出画面），助手送来的编码数据再经过和网络上一样的 NAL / OBU 边界检查。
- 远程控制默认关闭，由分享端在 `rcs_control_config` 里允许。每次授权变化都会更换“授权代次”，旧代次下产生的输入一律丢弃。撤销控制、断线或会话结束时，释放所有被按住的键和鼠标按钮。宿主还可以用 `on_input` 逐条否决输入，或者让 SDK 把输入全部交给宿主执行（`RCS_INPUT_HOST`）。
- 信任模型：共享密钥由宿主的业务通道传递，能读到这个通道的一方（例如宿主自己的服务器）也就能冒充任一端，这与宿主对自己服务器的信任一致。只用设备密钥（`RCS_SECRET_NONE`）时，中转的只有公钥，服务器即使被攻破也冒充不了任何一端——前提是宿主的信任列表来自可信的配对（例如首次连接时让用户核对，或者用口令完成第一次连接后记下对方的公钥）。网络上的其他人（运营商、中继、同一热点下的设备）只能看到密文。正式上线前仍建议做一次独立的安全评估。

## 依赖原则

- 核心只使用 C++20 标准库和系统框架（ScreenCaptureKit、VideoToolbox、AudioToolbox、CoreAudio、CoreGraphics、DXGI/D3D11、Media Foundation、WASAPI、CNG、Winsock），例外是 libopus、Monocypher（短 PIN 的 PAKE），以及系统没有密码学时的 BearSSL。
- 视频压缩用系统或显卡的编解码器，或随 SDK 编译的开源编解码库（BSD / MIT 类许可，`third_party/`），不自研。编进来的库各自是静态库，符号隐藏并加 `rcsdk_` 前缀（宿主自己也带同样的库时不冲突），每个都能用 `RCSDK_WITH_*` 去掉。
- 引入任何第三方库之前，要记录用途、许可证、包体积和替代方案。libopus 的记录如下：
  - 用途：默认的声音编码。实时场景里延迟最低、抗丢包最好，Windows 系统里没有 Opus 编码器。
  - 许可证：BSD，声明见 `THIRD_PARTY_NOTICES.md`。
  - 体积：源码 4.3 MB（去掉了 22 MB 的神经网络模型），编进库后几百 KB。
  - 替代方案：AAC（系统自带，编码延迟 40～60 ms）、PCM（不压缩，1.5 Mbit/s）。两者都保留为可选格式。
- 视频编解码库的记录（各自的 `README.rcsdk.md`）：
  - 用途：没有硬件编解码器的机器（虚拟机、云桌面）、系统没有的格式（VP8、VP9 各系统都不编，AV1 多数系统不编），以及硬件做不到的画面格式（VP9 / AV1 的 4:4:4 和 10 位）。libvpx 1.16.0：VP8 / VP9 编解码；dav1d 1.5.4：AV1 解码；libaom 3.15.2：AV1 实时编码；OpenH264 2.6.0：H.264（默认只在没有平台层的系统上编进去）。
  - 许可证：BSD（libvpx 3 条款加 Google 的专利授权，dav1d、libaom、OpenH264 2 条款，libaom 加 AOMedia 专利许可 1.0），声明见 `THIRD_PARTY_NOTICES.md`。专利：思科只为它自己发布的 OpenH264 二进制付 H.264 的专利费，从源码编的不带这个许可。
  - 体积：源码 libvpx 约 11 MB、libaom 约 21 MB、dav1d 约 9.5 MB、OpenH264 约 5 MB（只留编得到的部分）；macOS arm64 的发布版核心库从约 3.0 MB 到约 10.1 MB（前三个库）。
  - 替代方案：只用系统的（很多机器没有 VP9 / AV1 编码，虚拟机没有任何硬件编码）；FFmpeg（LGPL，体积大得多）；SVT-AV1（评估时实时码率控制不够）。
  - 构建：x86-64 要 NASM。libvpx 和 libaom 的配置用它们自己的 configure / CMake 按架构预先生成进仓库，构建 SDK 时不用 perl 和它们的构建系统；dav1d 和 OpenH264 的 CMake 照它们的 meson 写。
- BearSSL 0.6 的记录（`third_party/bearssl/README.rcsdk.md`）：
  - 用途：Android 的 NDK 没有系统的密码学和 TLS（`RCSDK_CRYPTO_BACKEND=bearssl`，这两个系统上的默认值；macOS 也能选，用来测试）。`crypto.hpp` 的全部原语和 TURN over TLS 的 TLS 1.2 客户端都由它提供，用它的常量时间实现；随机数仍取自系统（`arc4random_buf`、`getrandom`）。
  - 许可证：MIT，声明见 `THIRD_PARTY_NOTICES.md`。
  - 体积：源码 2.6 MB；静态库链进 SDK，只进用到的部分：macOS arm64 上整个库比用系统密码学时大 80 KB 左右（3.00 MB → 3.09 MB）。
  - 替代方案：自己实现（椭圆曲线、AES、X.509 和 TLS 不该自研）；OpenSSL 3、Mbed TLS（Apache 2.0 许可，不在允许之列，体积也大得多）；Android 的 Java 层（Conscrypt，NDK 代码要经 JNI 才能用）。
  - 代价：只有 TLS 1.2；BearSSL 在 ARM64 上只有软件的 AES（位切片）和 SHA-256：Apple M4 Pro 上 1200 字节的包，认证加密（AES-256-CTR + HMAC-SHA256）约 115 MB/s（系统的约 1 GB/s），2 核 ARM64 虚拟机里的转发器约一半的 CPU 花在这上面。
  - 所以 ARM64 上这两样用 SDK 自己写的 ARMv8 密码学扩展指令（`src/crypto/bearssl_armv8.cpp`，BearSSL 有 x86 的 AES-NI、POWER8 的指令，没有 ARMv8 的）：AES-CTR 用 AESE / AESMC（轮函数和密钥扩展的 SubWord 都是指令，没有查表，常量时间），SHA-256 用 SHA256H / H2 / SU0 / SU1，各是一个 BearSSL 的类（`br_block_ctr_class`、`br_sha256_context` 上的 `br_hash_class`），HMAC、PBKDF2、RFC 6979 都走它。处理器有没有这些指令：编译器能假定的（Apple 的 ARM64 都有），否则 Linux、Android 的 `getauxval(AT_HWCAP)`；没有时用 BearSSL 的软件（真实的能力退回），选的哪个在 debug 日志里写一次（模块 `crypto`）。上面那种包约 1.7 GB/s；对 NIST SP 800-38A、FIPS 180-4、RFC 4231 的向量，并和 BearSSL 的软件随机交叉检查（`tests/crypto_armv8.cpp`）。TLS（TURN over TLS 的 AES-GCM）还是 BearSSL 自己的。

## 扩展点

- **模块**：新功能默认做成模块（`rcsdk/module.h`，见 [MODULES](MODULES.md)），不进核心：两端互报、按观看端收发、机器那一半、事件、统计都由模块接口提供。官方模块放在 `modules/<名字>/`，一个库，只包含公开头文件。模块需要而接口没有的，加进模块接口（新的回调、服务函数或事件种类），所有模块一起得到。
- **宿主的编解码器**：`agent/host_encoder` 和 `session/host_decoder` 把宿主的回调包成平台的 `VideoEncoder` / `VideoDecoder`，视频管线和观看端按配置选用，下游分不出来；宿主直接给的码流（`session/host_sources` 的 `EncodedVideo`）走和助手的包同一条发送路径。包的检查（`protocol::videoProblem`）在发送前和收到时用同一个函数。新的“宿主能接管的东西”照这个办法加。
- **编解码器**：`EncodedPacket` 带有编解码器类型和参数集，协议和协商都与编解码器无关。新增一种格式，需要在各平台的 `VideoEncoder` / `VideoDecoder` 中实现（`open()` 拒绝未实现的编解码器或加速方式），把实现加入 `codecImplementations()`，并定义它的码流约定（例如 AV1 用 OBU，`config` 放序列头；VP8 / VP9 一帧原样，`config` 放 SDK 写的 `vpcC`）和 `protocol::videoProblem` 的核对。
- **编解码后端**：`rcs_backend` 区分同一平台上的不同接口（Media Foundation、NVENC、NVDEC、oneVPL、VideoToolbox）。工厂 `createVideoEncoder(config)`、`createVideoDecoder(config)` 按 `backend` 选：指定的就用它，`AUTO` 取实现列表里该 GPU 的第一个（Windows 上由 `nvencFirst`、`vplFirst` 判断；解码器 4:4:4 的流由 `nvdecFirst`、`vplDecodesFirst`）；编码器、解码器用 `backend()` 报告自己是哪个。各后端共用 `media/encoder_output.hpp` 把码流拆成参数集和画面。新增后端（例如 AMF）时，加一个枚举值，在实现列表中以这个后端报告它的实现，并在工厂里接上；公开接口只多一个枚举值。随 SDK 编译的库是 `src/codecs/`：`codecs::implementations()` 把系统的和库的实现合成一张表（每种格式和方向先硬件，软件里库在系统的前面，H.264 除外），`codecs::createEncoder` / `createDecoder` 按表里第一个匹配的选库或系统的工厂；库的编解码器只处理 `YuvImage`（CPU 上的 YCbCr），平台的 `SurfaceReader` / `SurfaceWriter` 负责和本系统的帧互换（macOS 原地读采集的像素缓冲，Windows 在 GPU 上转换后读回、解码后上传成纹理，Android 用 `AHardwareBuffer`）。新的库：`third_party/<库>/`、`src/codecs/<库>.cpp`、`libraries.hpp` 里三个函数和 `factory.cpp` 的一行、`RCSDK_WITH_*` 选项。
- **配置与运行中修改**：先看新东西属于哪一层（[API-DESIGN](API-DESIGN.md#5-以后的能力怎样加入) 第 5 节的规则）：宿主的决定是功能层的一个新 `RCS_EXT_*` 扩展（`rcsdk.h`）；引擎的参数加进对应领域的调优扩展（`tuning.h`）；宿主接管的一环是新的替换扩展，放在自己的头文件里，回调和 `user_data` 在扩展里，加进 `fixedExtensions`；能做成模块的做成模块。每种扩展在 `session/config.cpp` 的 `kinds` 表里加一行（解析校验；`preset` 标志决定预设给不给它值，给的话在 `presetSettings` 里写各个预设的取值），`session/checks.cpp` 核对本机能力。替换层之外的扩展都在运行中可改：运行中的读取通过 `Session::liveConfig()` 和 `settingsVersion()`，分享循环和观看端各自比较新旧设置决定重启什么；变化通过新的事件类型汇报，数据放进新的统计分区。
- **传输**：`protocol::Connection` 是接口，每种传输上都是数据报连接 `UdpConnection`（经转发器的观看端是两层合成的 `ForwardedConnection`）。数据报经由 `protocol::Link` 发出：直连、到对端的 TCP 连接（有序）、ICE 选出的路径（继续应答对端的检查）、TURN 中继；收发它们的 `transport::DatagramSocket` 是 UDP 套接字、分享端 TCP 端口上的一份（`TcpDemux`）、到分享端、TURN 服务器或转发器的 TCP 或 TLS 连接（`FramedSocket`：到分享端的长度分帧、`TurnTcpSocket`、转发器的长度和 WebSocket 分帧，字节流是 `ByteStream`：TCP，或系统的 TLS：macOS 的 Network.framework 在自己的队列上收发，收到的放在内存里、用一个管道叫醒网络线程的 `Waiter`，`local_interface` 经路径监视找到 Network.framework 的接口对象；Windows 的 Schannel 在 SDK 自己的非阻塞套接字上加解密，握手之后的消息（TLS 1.3 的会话票据、密钥更新）照 `SEC_I_RENEGOTIATE` 交回 `InitializeSecurityContext`，不阻塞。不用 Secure Transport：它已不再维护，只到 TLS 1.2；没有系统 TLS 的 Android 用 BearSSL 的 TLS 1.2 客户端（`transport/tls_bearssl.cpp`），像 Schannel 一样在 SDK 自己的套接字上加解密，信任的根证书读系统的证书文件），或者宿主自己的通道（`HostSocket`）。新的路径类型只需新增一种 `Link` 或 `DatagramSocket`；宿主想自己提供路径时，`RCS_TRANSPORT_HOST` 已经让它做到，不需要改 SDK。
- **Wi-Fi 不省电**：会话进入 `RUNNING` 时（`Session::setState`）按 `wifi_low_latency` 向平台要一个 `platform::lowLatencyWifi()` 的引用，离开 `RUNNING` 或运行中关掉时放手；一个进程里的会话共用一个（弱引用）。Android 是 `WifiManager` 的 `WIFI_MODE_FULL_LOW_LATENCY` 锁（经宿主交出的 Java VM，`android/network.cpp`），Windows 是 `wlanapi` 的句柄：每个连着的 Wi-Fi 接口设媒体流模式、关后台扫描，句柄关掉时系统自己恢复（`windows/network.cpp`）；Apple 的系统和其他系统 `UNSUPPORTED`（`other/network.cpp`），不写日志。失败只写日志、不影响运行，`rcs_network_stats.wifi_low_latency` 说是否保持着。
- **平台**：新增 `src/platform/<os>/`，实现 `platform.hpp` 的全部内容：屏幕采集、视频编码器和解码器、输入注入（按键表在 `platform/keys.hpp`）、声音采集和播放、AAC 编解码；系统查询（能力、编解码实现列表、显示器、权限）；帧访问（`withCpuImage`、`describeGpuFrame`）；以及桌面状态、共享的区域在屏幕上的位置（`sharedArea`）、Ctrl+Alt+Del 和被看护进程那一边。官方模块和组件的平台代码在它们自己的目录里（剪贴板、电源请求、插入点的读取器、看护者）。系统没有的能力也要实现，抛出或返回 `UNSUPPORTED` 并说明原因（参考 `other/desktop.cpp`、`unsupported/platform.cpp`）。
- **助手**：`agent::Port` / `agent::Events` 是会话和助手之间的边界。新的机器能力（跟随安全桌面就是这样加的）加在 `MediaAgent` 里，在接口和链接消息上各加一项；会话不关心助手在哪个进程。宿主按会话开关的机器能力先考虑做成模块的机器那一半（保持唤醒就是这样，见上文），不用动链接。
- **看护者**：组件 `components/supervisor` 里 `supervisor::Supervisor` 是接口，Windows 实现在 `src/supervisor_windows.cpp`，其他系统是 `src/none.cpp`（`UNSUPPORTED`）；macOS 的 launchd 不需要它。新的看护方式：另一种令牌是新的 `rcs_helper_account` 枚举值；会话由 `os_session` 选（0 跟随控制台），运行中可改的设置都在 `supervisor::Settings` 里，`rcs_supervisor_update` 只改它们。会话之外的功能（不进会话、不用模块接口的）做成这样的组件：`components/<名字>/`，一个库，只用公开头文件；它要而核心没有的公开函数（日志、调用失败的原因），加进 `rcsdk.h` 给所有建在 SDK 上的库用。
