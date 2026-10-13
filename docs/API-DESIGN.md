# 接口设计记录

公开接口分三层：入门和功能在 `include/rcsdk/rcsdk.h`（C）和纯头文件的 C++ 包装 `include/rcsdk/rcsdk.hpp`；调优在 `rcsdk/tuning.h`；替换（宿主接管一环）各有自己的头文件 `rcsdk/host_codec.h`、`rcsdk/rate_control.h`、`rcsdk/host_transport.h`、`rcsdk/gpu.h`、`rcsdk/agent.h`；写模块用 `rcsdk/module.h`；系统自己的类型经各自的头文件交接（`rcsdk/apple.h`、`rcsdk/android.h`，别的系统上那里的函数是 `UNSUPPORTED`）。C++ 各有同名的 `.hpp`。本文记录接口为什么是现在这个样子、版本怎么管理，以及以后的能力怎样加进来。用法见[开发者指南](guide/README.md)，每个字段的说明以头文件为准。

## 1. 原则

1. **开箱即用，也能深度定制。** 开发者接入时只填核心配置（角色、地址、密钥）就能跑通，默认值按一般情况均衡取舍。有特殊需求时，每一项行为都能通过扩展调整；SDK 内部策略的每次变化都能在事件和统计里看到。
2. **只提供底层能力，接口要丰富。** SDK 负责数据面（采集、编解码、传输、加密、码率、输入、声音），控制面（身份、配对、授权、信令通道、界面）归宿主。SDK 不带界面、不替宿主做产品决定，但把宿主可能想调整、想观察的东西都开放出来，减少宿主自己写底层代码的工作。
3. **不悄悄替换能力。** 所有选择都可以通过查询发现；做不到时返回明确的错误码和原因，由宿主决定下一步。
4. **1.0 之前不做兼容。** 见第 3 节。
5. **给以后的能力留位置。** 新能力以新的扩展、事件类型、统计分区、函数或枚举值的形式加入，不把宿主可能想改的策略写死。
6. **热路径不走通用机制。** 视频帧、声音和数据消息走专用回调，事件和统计不进入每帧路径。

## 2. 结构

| 部分 | 接口 |
| --- | --- |
| 核心配置 | `rcs_session_config { struct_size, version, role, preset, connect, identity, extensions }`：角色、预设（第 6 节）、怎么连（`connect`：传输方式、地址、端口）、怎么证明身份（`identity`：密钥、口令或 PIN）。运行中换身份用 `rcs_session_set_identity`，换预设用 `rcs_session_set_preset` |
| 扩展 | 以 `rcs_extension { type, struct_size, next }` 开头的结构体，按层和角色分组编号。功能：分享端 `SHARE_VIDEO`（画面、码率、对要求的处理）、`ENCODER`、`CAPTURE`、`STREAMS`（更多显示器，每块一路）、`CONTROL`、`VIEWERS`；观看端 `VIEW_REQUEST`、`DECODER`；两端 `SEND_AUDIO`、`RECEIVE_AUDIO`（声音按方向）、`ICE_SERVERS`（P2P 和中继的服务器）、`RECONNECT`（连接断了运行怎么办）、`BACKGROUND`（iOS 进后台时暂停什么，观看端和分享端都用）、`DEVICE_IDENTITY`（设备密钥和信任的公钥）、`LOG`、`MODULE`。调优（`rcsdk/tuning.h`）：`VIDEO_TUNING`、`AUDIO_TUNING`、`NETWORK_TUNING`。替换（各自的头文件）：`HOST_ENCODER`、`HOST_DECODER`、`ENCODED_OUTPUT`（编码包交给宿主，两端）、`RATE_DECIDER`（宿主决定码率）、`HOST_TRANSPORT`（宿主的通道）、`GPU_DEVICE`（在宿主的 D3D11 设备上解码）、`AGENT_LINK`（助手在另一个进程）。宿主没放进链的扩展取预设的值，放进了的完全是宿主的。助手的配置也有扩展链，接受其中适用于助手的（`LOG`、`HOST_ENCODER`） |
| 运行中修改 | `rcs_session_update(session, changes)`：停止时可改本角色的任何扩展；运行中入门、功能和调优层的都能改，大多立即生效，`DEVICE_IDENTITY`、`ICE_SERVERS` 的服务器和凭据、`NETWORK_TUNING` 的网卡从下一个连接起生效（`ICE_SERVERS` 的 `turn_transport` 在一次运行中固定）；替换层的扩展在一次运行中固定。整条链全部生效或都不生效。`rcs_session_get_settings` 读出任意一个扩展现在生效的设置（预设给的也一样） |
| 事件 | `on_event(const rcs_event*)`：`STATE`（`FAILED`、`RECONNECTING` 和再次 `LISTENING` 时带原因）、`PEER`、`VIEWER`（分享端的观看端加入、离开）、`STREAM`（带变化位）、`CODEC`、`CAPTURE`、`DISPLAYS`、`RECOVERY`、`KEYFRAME`（宿主直接给码流时该出关键帧了）、`RATE`、`CURSOR`、`AUDIO`、`CONTROL`、`DATA`（发送队列又有空间）、`AGENT`（助手接上、离开及原因）、`DESKTOP`（用户桌面或安全桌面）、`PIN`（PIN 握手失败）、`ADMISSION`（准入的等待和结论）、`MODULE`（模块激活与否和模块自己的事件）、`PAUSE`（暂停了什么、谁暂停的）。事件按含义编号，共 20 种 |
| 统计 | `rcs_session_get_stats(session, section, &stats)`：分享端 `CAPTURE`、`VIDEO_SEND`，观看端 `VIDEO_RECEIVE`，两端 `AUDIO_SEND`、`AUDIO_RECEIVE`（声音按方向）、`NETWORK`、`INPUT`、`DATA`；一对多时按观看端的 `rcs_session_get_viewer_stats`，多路画面按路的 `rcs_session_get_stream_stats`，模块的 `rcs_session_get_module_stats` |
| 信息 | `rcs_session_get_state`、`rcs_session_get_failure`、`rcs_session_get_settings`、`rcs_session_get_stream_info`（正在传的画面和声音）、`rcs_session_get_peer_info`（对端的平台和它提供的格式）、`rcs_session_get_control_info`、`rcs_session_get_desktop_info`；分享端的 `rcs_session_get_agent_info`、`rcs_session_list_displays`、`rcs_session_list_windows`、`rcs_session_list_viewers` |
| 动作 | `rcs_session_signal`（信令）、`rcs_session_send_input`、`rcs_session_request_keyframe`、`rcs_session_pause` / `rcs_session_resume`；宿主自己的画面和声音：`rcs_session_push_video_frame`、`rcs_session_push_gpu_frame`、`rcs_session_push_sample_buffer`（`apple.h`）、`rcs_session_push_hardware_buffer`（`android.h`）、`rcs_session_push_audio`；宿主自己的码流：`rcs_session_push_encoded_video` / `rcs_session_video_ready`，`rcs_encoder_output_push`（宿主的编码器交回包）；一对多：`rcs_session_admit_viewer`、`rcs_session_disconnect_viewer`、`rcs_session_set_viewer_control`、`rcs_session_set_main_viewer`；宿主的通道：`rcs_session_transport_receive` / `rcs_session_transport_closed`；`rcs_session_send_data`（数据通道，对端的 `on_data` 收到）、`rcs_session_stop_with_reason`（告诉对端“我会回来”） |
| 其他对象 | 助手 `rcs_agent_*`（分享端在另一个进程里的采集和编码）、看护者 `rcs_supervisor_*`（组件 `rcsdk/supervisor.h`、`RemoteControl::Supervisor`，不在核心里：Windows 服务在控制台会话或宿主选的会话里保持一个助手进程，`rcs_supervisor_update` 运行中换会话；`rcs_list_os_sessions` 列出可选的会话）。每个对象都是同一种模型：`*_create` / `start` / `stop` / `destroy`，`on_event` 收带类型、时间戳的事件结构，查询函数返回当前值 |
| 实现列表 | `rcs_list_codec_implementations`：编码格式、方向、硬件 / 软件、后端、GPU LUID、最大画面、画面格式、名称；`rcs_get_capabilities` 是它的摘要，另外报告屏幕（含采集能产生的画面格式）、声音和输入能力 |
| 失败原因 | `rcs_get_last_call_error`：调用被拒的原因（包括 `start` 拒绝的配置）；运行失败用 `rcs_session_get_failure` |
| 结果码 | 按含义分组：调用本身（参数、状态、能力、线程、缓冲、队列、超时、取消、内部）、系统（权限、采集、编解码、声音设备）、对端和路径（网络、认证、协议、对端关闭、对端重启、被替换、被拒绝）。`INVALID_ARGUMENT` 只用于本版本没有定义的值或越界；`UNSUPPORTED` 只表示这台机器或这个角色做不到 |
| 日志 | `rcs_set_log_callback`：进程级的一个回调，每条记录（`rcs_log_record`）写明属于哪个会话、助手或看护者，带着那个对象的 `user_data`；`RCS_EXT_LOG` 给单个会话或助手自己的级别 |
| 版本 | `RCSDK_VERSION_MAJOR` / `MINOR` / `PATCH` 和 `RCSDK_VERSION`（头文件），`rcs_version_number()` / `rcs_version()`（实际加载的库），核心配置的 `version` |
| C++ | `rcsdk::SessionConfig(role, preset)`、`rcsdk::AgentConfig` 组装扩展链（`set*` 设置，名词返回整个扩展结构体，`extension<T>()` / `use(ext)` 放进别的层的扩展，从预设的值开始）；`rcsdk::Session`、`Callbacks`、`GpuFrame`；替换层在各自的 `.hpp` 里：宿主实现的接口 `HostEncoder`、`HostDecoder`、`EncodedOutput`、`RateDecider`、`HostTransport`、`AgentGate` 和组装扩展的函数，`Agent`；`Supervisor` 在组件里；`Session::stats<T>()`、`settings<T>()`、`update(ext...)` / `update(config)`、`setPreset`、`setIdentity`、`failure()`。统一规则：名词返回所指的东西，`set*` 修改设置，动词执行动作；动作返回 `rcs_result`，构造、设置和产生新东西的名词抛 `rcsdk::Error`，会话的状态和信息查询 `noexcept` |

结构体内的字段也按含义分组（例如能力查询先列屏幕和声音，再列编解码；统计先列状态和名称，再列计数，最后是耗时），不按加入的先后排列。

## 3. 版本规则（1.0 之前）

SDK 还在开发阶段，接口要能随时改到合理为止，所以 1.0 之前**不保留任何兼容代码**：

- **一个版本号。** SDK 的版本 `0.MINOR.PATCH` 同时标识头文件和库，写在 `rcsdk.h` 里（`RCSDK_VERSION_MAJOR` 等），构建从那里读取，不在别处重复。
- **每个版本都可以有破坏性修改**：结构体字段重排、改名、删除，枚举重新编号，默认值改变，网络协议升级。改动和迁移方法写进 [CHANGELOG.md](../CHANGELOG.md)；同一个提交里同时更新示例、测试和文档。
- **头文件和库必须是同一个版本**，不同时明确报错，不去猜：
  - `RCS_SESSION_CONFIG_INIT` 把 `version` 填成头文件的 `RCSDK_VERSION`，与库不同时 `rcs_session_create` 返回 `INVALID_ARGUMENT`，原因里写明两个版本。
  - 每个结构体的 `struct_size`、列表函数的 `item_size` 必须正好等于本版本的 `sizeof`，否则返回 `INVALID_ARGUMENT`。它们只用来发现“头文件和库不是同一版本”或“没有用 `*_INIT` 初始化”，库从不按前缀读写。
  - 库在运行时才加载时（例如 Windows 上放在应用目录里的 DLL），宿主可以在启动时比较 `rcs_version_number()` 和 `RCSDK_VERSION`。
  - macOS 的动态库按次版本命名（`librcsdk_core.0.<MINOR>.dylib`），装错版本时系统加载就会失败；CMake 包只匹配同一个次版本。
- **两端运行同一个版本。** 网络协议的版本写在握手第一条消息的协议标记里（`src/transport/handshake.hpp`；现在是 v1），线上格式每变一次就加一。协议版本不同的两端握手失败：分享端回一个只含自己协议标记的通知、丢弃这个连接、在日志里写明“协议版本不匹配”并计入 `rejected_connections`，观看端以 `UNSUPPORTED` 失败并写明两端的协议版本。助手链接有自己的明文前缀（链接标记、格式号、`RCSDK_VERSION`），版本不同时助手以 `UNSUPPORTED` 结束、不再重试。SDK 不尝试和旧协议通信。
- **不认识的值就是错误。** 不存在的扩展类型、枚举值、统计分区一律返回 `INVALID_ARGUMENT`，它们只可能来自写错的代码或另一个版本的头文件。`UNSUPPORTED` 只表示“这台机器（或这个角色）做不到”，例如没有某种硬件编解码器。

1.0 起才承诺兼容，具体规则（哪些改动允许、怎样升级主版本）到时候写进这一节。

## 4. 决定

1. **扩展链，而不是“配置句柄 + 设置函数”**（与 Vulkan 的 `pNext` 相同）：
   - C 用户可以把结构体放在栈上，不需要额外的分配和释放。
   - 一个扩展就是一个普通结构体，其他语言绑定时直接映射。
   - 同一套结构体还能直接用于 `rcs_session_update`。
   - C++ 用户有 `SessionConfig` 构建器，不用碰指针。
2. **事件回调在 SDK 的工作线程上执行**，与帧回调规则一致；SDK 保证事件之间不重叠，由宿主自己派发到界面线程。不另设宿主派发函数，以免 SDK 依赖宿主的事件循环。
3. **声音与视频共用一条连接**：同一个握手、同一个加密通道，UDP 上声音是单独的数据报类型，不重传。
4. **后端是 `rcs_encoder_config` / `rcs_decoder_config` 的 `backend` 字段，不按实现编号选择。** “编码格式 + 硬件 / 软件 + 后端”已经确定一个实现；编号在不同机器和驱动版本之间不稳定，不适合写进配置。Windows 上分享端的硬件编码器由被共享显示器所在的 GPU 决定，实现列表中的 `adapter_luid` 说明了这一点。
5. **扩展按角色划分，每个扩展描述一件事的全部设置**。分享端：`SHARE_VIDEO`（发送的画面、码率怎样跟随网络、怎样对待观看端的要求）、`ENCODER`、`CAPTURE`（来源、显示器、隐藏自己的窗口）、`CONTROL`；观看端：`VIEW_REQUEST`（对画面、声音和指针的要求）、`DECODER`（格式、实现）。（输入加速、解码的积压上限这类引擎参数在调优层的 `VIDEO_TUNING`，D3D11 设备在替换层的 `GPU_DEVICE`。）声音按方向分：`SEND_AUDIO`（发出的声音）和 `RECEIVE_AUDIO`（收到的声音）两种角色都能用，各角色有自己的默认。一端不会配置到对它没有意义的字段；运行中修改时一个扩展整体替换，“哪些能改”不再按字段区分，只有传输方式固定。
6. **宿主自己的画面、声音和输入是配置里的来源 / 目标，不是另一套接口**：`CAPTURE.source`、`SEND_AUDIO.source`、`CONTROL.target`，加上推送函数和 `on_input`。SDK 内部的采集、声音和输入执行各是一个接口，宿主提供的实现和系统的实现并列。
7. **对端的信息单独一个结构体**（`rcs_peer_info`）：平台、提供的格式、声音开关，连上时和对端运行中修改时通过 `RCS_EVENT_PEER` 汇报，不混进 `rcs_stream_info`。
8. **事件说明变了什么**：`RCS_EVENT_STREAM` 带变化位，宿主不必自己比对。
9. **默认值**：H.264（几乎所有 GPU 都能硬件编解码）、1920×1080、60 FPS、20 Mbit/s、UDP（动态码率）、声音开启（Opus）、不允许控制；分享端自己的窗口和声音照常共享，隐藏和排除都是宿主可以打开的选项。
10. **一次运行经得起连接断开，但不替宿主决定何时放弃**：分享端默认一直监听、观看端默认重连 30 秒，都是 `RECONNECT` 扩展里的设置；每次连接变化通过状态事件和原因报告，“对端关闭了连接”（`DISCONNECTED`）和“路径断了”（`NETWORK`）是两个错误码，因为宿主对它们的处理不同。第一次就连不上、P2P 连通检查失败这类“没连上过”的失败照旧结束运行，由宿主决定下一步。
11. **数据通道是消息，不是流**：`rcs_session_send_data` 发一条条带通道号的消息（可靠有序或不可靠），SDK 不解释内容；标注、宿主的命令这些建立在它上面，或者做成模块（见 34；剪贴板和文件传输就是官方模块，见 30 和 [FILE-TRANSFER-DESIGN](FILE-TRANSFER-DESIGN.md)）。可靠消息排在输入之后、画面之前：和控制消息共用有序流，但一次只发一条、在路上的有上限，所以输入最多等一小段，视频的节流和码率不受影响。
12. **身份是机制，信任是宿主的决定**：SDK 只在握手里证明（共享秘密、设备密钥签名）和核对（对端公钥在不在宿主给的列表里），并报告对端出示的公钥；不保存任何密钥、口令或可信设备列表，不做配对流程和界面。私钥是宿主传进来的 97 字节（SEC 1），宿主存在系统的钥匙串或凭据库里。密码学用系统的；短 PIN 需要的曲线运算系统不提供，用了 Monocypher（见第 29 条）。
13. **分享端分成会话和助手，助手的位置是配置，不是另一套接口**：采集、编码、声音采集和输入执行默认在会话进程里；挂上 `RCS_EXT_AGENT_LINK` 后由另一个进程里的助手（`rcs_agent_*`）做，会话的接口、设置、事件和统计都不变。助手没有自己的采集或编码配置，所以宿主只在一个地方设置画面，也只在一个地方修改。SDK 不决定助手怎么启动（服务、登录项、LaunchAgent 由宿主做），也不替宿主决定助手离开多久就放弃：它报告 `RCS_EVENT_AGENT`，宿主自己计时。链接沿用网络上的安全通道，而不是只靠系统的访问控制：本机的其他进程即使能打开端点，没有密钥也通不过握手。
14. **无人值守是几块通用的积木，布置由宿主选**（Windows）：`rcs_capture_config.secure_desktop`（采集和输入跟随登录屏、锁屏、UAC，默认关）、`RCS_EVENT_DESKTOP`、输入类型 `RCS_INPUT_SECURE_ATTENTION`、看护者 `rcs_supervisor_*`、保持唤醒（官方模块 `rcsdk.power`），以及停止原因 `RCS_STOP_RESTART`。SDK 不规定服务怎么装、助手怎么布置：被看护的进程可以就是整个分享会话（方案 A），也可以是留在服务里的会话的助手（方案 B），两种用同一套接口，示例 `examples/unattended` 用 `--mode a|b` 选。做不到的（不是 LocalSystem、不是 Windows）直接返回 `UNSUPPORTED` / `PERMISSION_REQUIRED` 并说明，不退回成暂停；系统策略（`SoftwareSASGeneration`）、防火墙、服务的注册都是宿主的事，SDK 只读不改。登录屏、锁屏和 UAC 在 Windows 里是同一个桌面，所以桌面状态只分用户桌面和安全桌面两类，不猜。
15. **“我会回来”是停止的原因，不是另一种连接**：`rcs_session_stop_with_reason` 的 `RCS_STOP_RESTART` 让对端报告 `RCS_ERROR_RESTARTING`，观看端据此立即重连，分享端回到监听；`RCS_ERROR_DISCONNECTED` 仍然是“它走了”，不重连。通知是连接上的最后一条消息，进程崩溃来不及发，对端按原来的超时报告 `NETWORK`。
16. **助手链接的信任由系统核实，由宿主决定**：两端的进程身份（进程号、会话、账户）来自操作系统，从不由对端自报；会话一侧可以在 `on_agent_attach`（在 `RCS_EXT_AGENT_LINK` 里）拒绝，助手一侧按 `access` 只接受特定账户的会话；看护者可以把一次性的链接密钥经继承的句柄交给它启动的助手（`helper_key`），磁盘上不必有密钥。新助手顶替旧助手是终止（`RCS_ERROR_REPLACED`），由宿主决定是否再启动，SDK 不在两个助手之间来回。
17. **常驻监听的分享端不会被观看端结束**：观看端或路径造成的任何失败（连通检查失败、没有共同格式、违反协议、握手失败）都回到 `LISTENING` 并说明原因；只有分享端自己的失败结束运行。违反协议是单独的结果码 `PROTOCOL`，观看端不对它重连。
18. **所有回调都是“回调线程”**：SDK 在调用宿主的每一个回调时标记这个线程，从任何回调里做生命周期调用都返回 `CALLBACK_THREAD`，而不是按“哪些线程会发事件”逐个标记。
19. **宿主可以自己提供传输，SDK 留着连接本身**：`RCS_TRANSPORT_HOST` 让 SDK 的连接走宿主已有的通道，交接点是数据报（或宿主不必懂格式的字节流），而不是“消息”或“流”：加密、重传、节流、动态码率、重连仍是 SDK 的，宿主不需要实现任何协议，也看不到内容。接口是一个回调和两个函数（`on_transport_send`、`rcs_session_transport_receive`、`rcs_session_transport_closed`），通道的形态（`host_channel`）和大小（`host_max_datagram`）和回调一起在替换层的 `RCS_EXT_HOST_TRANSPORT`（`rcsdk/host_transport.h`）里；回调返回 0 就是背压，不另设事件。SDK 不内置 WebRTC、QUIC、WebSocket 的适配：那需要依赖，也会替宿主选框架。
20. **日志按对象归属，回调仍只有一个**：每条日志是一个结构体（以后加字段不改签名），写明属于哪个会话、助手或看护者，带着那个对象回调表里的 `user_data`，宿主按它分流，不需要另建映射；每个会话和助手可以有自己的级别（`RCS_EXT_LOG`，运行中可改）。不设“每个会话一个日志回调”：来源已经在记录里，两套回调反而要说明先后和重复。归属跟着线程走：SDK 启动的线程继承启动它的线程的归属，交给系统、在系统线程上调用的回调绑上交出者的归属。助手的配置为此也有了扩展链，以后交给助手的功能（例如宿主的编码器）走同一条链，而不是在助手的配置里加字段。
21. **测量归 SDK，码率可以由宿主决定**：动态码率的测量（报告、排队延迟、丢包、到达速率）只有 SDK 做得到，所以留在 SDK；决定分成内置规则（阈值和升降速度全部开放在 `RCS_EXT_VIDEO_TUNING` 里，运行中可改）和宿主的 `on_rate_report`（在 `RCS_EXT_RATE_DECIDER` 里）：每份报告带着全部测量值和规则的建议，宿主改建议就是自己的决定，不改就是只看，所以“调参数”“看依据”“换算法”是同一个接口的三种用法，不另设事件。决定只管码率，用结构体传，以后加帧率、分辨率不改签名。
22. **编解码可以交给宿主，其余仍是 SDK 的**：宿主的编码器、解码器各是一个扩展加几个回调，宿主直接给码流是一种画面来源；协商、传输、动态码率、关键帧请求和恢复都不变。标准格式的码流照原有约定，所以两端可以一端宿主、一端系统；自定义格式用编号 16..31，和现有的协商、32 位掩码一致，而不是字符串名字。宿主的解码器自己显示画面，SDK 不再交付。编码在助手里时，宿主的编码器经助手的扩展链交给助手。编解码器的回调在一次运行中固定，不能运行中替换。
23. **共享什么是一种画面来源，看不到时暂停而不是结束**：显示器、区域、窗口、应用和宿主的画面、码流一样是 `rcs_capture_config.source` 的值，各自需要的字段（`region_*`、`window_id`、`process_id`）放在同一个结构体里，按含义分组；不为“窗口采集”另设扩展或会话类型，运行中换共享的东西就是 `rcs_session_update` 一个 `CAPTURE`。共享的窗口最小化、被关掉、应用退出都是暂停加原因（`rcs_capture_pause`：`HIDDEN`、`GONE`），连接和观看端都留着，换什么、要不要停由宿主决定；`GONE` 不会自己恢复，因为窗口号和进程号会被重新使用。SDK 只给窗口列表（标题、应用、位置、是否最小化、是否在前），不做选择界面。远程输入跟着共享的东西走：共享窗口或应用时只进它们，被挡住时先把它提上来，因为观看端看不到别的窗口，输入落到看不到的地方是错的，这不是宿主该调的策略。
24. **同一块 GPU 可以有几个后端，`AUTO` 取实现列表的第一个**：NVIDIA GPU 上直接调 NVENC 和经 Media Foundation 都是这块 GPU 的编码器，是同一个“编码格式 + 硬件”的两个实现，所以用后端区分，不另设扩展或开关。列表里每块 GPU、每种格式按“好的在前”排列，`AUTO` 就是取第一个，排序的依据写在 PERFORMANCE 里，靠实测改，不靠猜；宿主想要别的就填后端。指定的后端在这块 GPU 上没有时失败并说明，不换成别的后端，和“没有静默回退”一致。第三方的接口声明（`third_party/nvenc`、`nvdec`、`onevpl`，MIT）随源码提供，驱动在运行时加载，没有就不列出，SDK 不因此多一个链接依赖。Intel GPU 上直接调 oneVPL 排在 Media Foundation 前面（实测每帧编码更快）；NVDEC 是解码器，只接 4:4:4，排在 Media Foundation 的硬件解码器后面，`AUTO` 时按流的画面格式交给它。oneVPL 也解码，排在 NVDEC 后面：它解 4:2:0 也解 4:4:4，但同一块核显上 Media Foundation 解 4:2:0 每帧快 0.5～3 ms，所以 `AUTO` 只把 4:4:4（NVIDIA GPU 上先给 NVDEC）和 Windows 没装解码器的编码格式交给它，4:2:0 宿主可以指定。
25. **输入按类型扩展，能不能执行由能力位说**：新的输入是 `rcs_input_type` 的新值，字段放在同一个 `rcs_input_event` 里按含义排（位置、相对量、键码、滚动、压力和倾斜），不另设事件结构。滚轮分成 `WHEEL`（整行，有刻度的滚轮）和 `SCROLL`（1/120 行，触控板），而不是改 `WHEEL` 的单位：两种设备在两个系统里本来就是两种事件（macOS 的行和像素，Windows 的刻度和高精度），已经按行发滚轮的宿主不受影响。媒体键用 HID 消费者页（`RCS_KEY_CONSUMER | 用法码`），和键盘页一样按“哪个键”而不是“哪个系统的虚拟键”命名。文本是单独的类型，一个 Unicode 字符一个事件，和按键分开：按键是位置，文本是结果，输入法和表情只有结果。本机能执行哪些在 `rcs_capabilities.input_types`，观看端从 `rcs_peer_info.input_types` 知道对端收哪些；执行不了的报 `UNSUPPORTED`，SDK 不替宿主把触摸换成鼠标，这是宿主的交互决定。
26. **缩略图按“共享什么”取，而不是另一套描述**（`rcsdk/module.h` 的 `rcs_capture_picture`，到会话的助手那里取是缩略图模块）：取图的函数收的是会话用的同一个 `RCS_EXT_CAPTURE` 扩展，所以显示器、区域、窗口、应用都能取，检查（来源、授权、显示器、区域）和会话的采集是同一段代码，采集会暂停的情况就是 `INVALID_STATE` 加原因，选择界面据此标出“最小化了”。缓冲区是调用方的，大小的上限由调用方给的 `max_width × max_height` 决定，一次分配就够；SDK 不缓存、不定时刷新，多久取一次是宿主的事。Windows 一个进程只能复制一个显示器一次，所以正在采集的显示器由那个采集来取，而不是失败或悄悄换另一种采集方式；做不到的字段（Windows 上一帧的 `exclude_current_process`）是 `UNSUPPORTED`，不忽略。
27. **执行者按类型分，输入法只缺插入点**（插入点是官方模块 `rcsdk.text-caret`）：`rcs_control_config.host_input_types` 把某几类输入交给宿主、其余仍由系统执行，而不是只能在“全部系统”和“全部宿主”之间选：宿主把触摸或笔交给自己的画布时，键鼠仍由 SDK 注入。手柄暂不做：用处小，系统执行要么只在开发者模式下能用（Windows 的注入接口），要么要装内核驱动（ViGEm）或向苹果申请权限，跨平台成本高（见 ROADMAP）。输入法的组字留在观看端（本地输入法知道本地键盘和用户），SDK 只补上远端的文本插入点：观看端要、有控制权时才看（UI Automation 会让一些程序打开无障碍支持，有开销），只报变化，坐标和输入同一个空间。
28. **一对多是一个流、多个连接**：采集只做一份，大家跟得上时编码也只做一份，同一份码流发给每个观看端，而不是每人一份编码；有人跟不上时再多编一档给它（`RCS_EXT_ENCODINGS`，见[指南 07](guide/07-sharing.md#弱的观看端多一档编码rcs_ext_encodings)）；所以画质、指针模式只能有一个决定者（主观看端，宿主可以指定），码率跟随谁是宿主的选择（`bitrate_from`），慢的观看端不拖住别人：它自己的发送队列丢掉积压、从关键帧继续。观看端的编号进事件的信封（`rcs_event.viewer_id`）、数据消息和输入事件，而不是另设一套“发给某个观看端”的函数：0 表示全部或不针对谁。加入是宿主的决定（`on_viewer_join`，和 `on_rate_report` 一样预填默认、改了就是决定），能不能服务是 SDK 的判断（`RCS_ERROR_BUSY`，原因写明），不替宿主排队或踢人。要问用户的宿主之后再决定（`RCS_ADMIT_LATER` 和 `rcs_session_admit_viewer`）：回调照样马上返回，等待的观看端保持连接、占名额、两端都有事件，界面和流程是宿主的；被拒绝是自己的结果码 `REFUSED`，和认证失败分开。（默认 64 个；P2P 和中继上见第 31 条；HOST 上也是，按宿主给每条通道的路由号分开。）
29. **短 PIN 用 PAKE，次数限制是分享端的一个字段**：`RCS_SECRET_PIN` 是 `rcs_secret` 的新值，PIN 放在 `password` 里，而不是另一个扩展：它和口令一样是“两端都知道的一段文字”，只是证明方式不同。证明用 CPace（CFRG 的 PAKE 草案，CPACE-X25519-SHA512）：它是平衡的 PAKE，两端都只需要 PIN，不需要像 OPAQUE 那样先注册、存储服务端的记录，正好对应“一端显示、一端输入”；只要一次往返，正好放进现有握手的两条问候消息里（各加 32 字节的份额），握手的其余部分（ECDH、设备密钥、finished 校验）一点不变，ISK 只替换连接密钥原来的位置。X25519 和 Elligator 2 系统密码学不提供，所以引入了 Monocypher（BSD-2-Clause / CC0，自己的代码之外只有这一处需要它，符号改名隐藏）。PAKE 只能把离线猜变成在线猜，限制在线猜的次数才是安全性的另一半：次数（`pin_attempts`，默认 5）是 `rcs_identity` 里的字段而不是固定值，宿主按自己的场景定；锁住后不自动解锁，也不按时间恢复，由宿主重新设置身份（同一个 PIN 也行）来决定，SDK 只报告每次失败（`RCS_EVENT_PIN`）。正在进行的握手占着一次尝试，所以并发的猜测也不能超过次数。观看端不计数：它只连宿主让它连的地址，是否再试由宿主和用户决定。
30. **剪贴板是 SDK 的能力，不是数据通道上的约定**（官方模块 `rcsdk.clipboard`，见 34）：读写系统剪贴板和输入一样要和系统打交道（Windows 的剪贴板和它的属主窗口、macOS 的粘贴板、格式转换、密码标记），无人值守时还必须在助手的用户会话里做，所以属于 SDK 的数据面；宿主只定策略：开不开（默认关，剪贴板里可能有密码）、哪个方向、哪些格式、多大、密码内容发不发，以及在 `on_clipboard` 里逐份放行。方向由两端各自决定自己的机器（发和收是两个位），而不是一个“同步”开关：远程协助常常只许一个方向。每种格式在线上只有一种形式（UTF-8 LF 文字、HTML 文档、RTF、PNG），系统之间的差别（CRLF、CF_HTML 头、位图和 TIFF）在两端各自的平台层里换，宿主和对端都不用知道。本端收什么由模块的问候告诉对端，运行中变了就重发，所以发送端只发对端收的格式和大小，不在对端丢。内容是模块消息（`RCS_MODULE_REPLACE`：新内容覆盖还没发出的旧内容），有自己的队列和统计，不占宿主的通道号和队列。宿主也可以接管（`target` HOST）：模块不碰系统剪贴板，宿主自己发、自己放。文件不在这一项里：它是文件传输模块（`rcsdk.file-transfer`，见 [FILE-TRANSFER-DESIGN](FILE-TRANSFER-DESIGN.md)）。
31. **公网上的一对多：每个观看端一次 ICE，信令带宿主自己的路由号**（见[指南 09](guide/09-transports.md#p2p-和中继上的多个观看端)）：观看端先给信令，分享端为每一份单独收集候选地址（中继：单独分配）、单独回答，而不是分享端给一份信令、所有观看端共用它的套接字和中继分配。共用的做法要在一个套接字上按 STUN 用户名、事务号和地址把多个 ICE 会话和已建立的连接分开，一个中继分配要转发给所有观看端（coturn 的 `max-bps` 按分配限速，就成了所有观看端的总和），常驻监听的分享端的反射地址和分配还要一直保活，否则宿主手里的信令会过时。逐个回答时，每次都是新收集的候选和新的分配，不会过时；每个观看端的路径、分配和失败只属于它自己；现有的 ICE、TURN 客户端、UDP 连接和每个观看端的那一套原样用上。代价是分享端在收到观看端的信令之后才收集（多一个往返左右），每个观看端占分享端一个端口和一个分配。路由号是宿主自己的 `uint64_t`（`rcs_session_signal` 带进来，分享端的 `on_signal` 原样带回，加入后在 `rcs_viewer_info.signal_route`），而不是 SDK 生成的编号：宿主不用维护“编号 → 谁”的表，还能把加入的观看端和自己知道的身份对上；文本里不放路由，因为文本对宿主是不透明的。哪两份信令是一对，由信令自己说（分享端的信令写明它回答的是哪一份），不靠先后，因为宿主的通道可以乱序、重复、送来以前的。上限由宿主决定：默认 64，最多 256（`RCS_MAX_VIEWERS`：一台 M4 Pro 上实测过 256 个，512 个时同一台机器上的观看端自己就占满了 CPU，没能测），每个观看端的开销写进文档；SDK 只在系统给不出资源时拒绝（`BUSY`，原因写明）。HOST 上的多个观看端按宿主给每条通道的路由号分开（第 28 条）。
32. **画面格式和编码格式一起协商，组合是一个枚举值**：色度采样、位深和动态范围合成一个 `rcs_picture_format`（4:2:0 / 4:4:4 × 8 / 10 位 × SDR / HDR10），而不是三个开关，因为实现的能力本来就是按组合给的（Windows 上 NVENC 的 HEVC 做得了 8 位 4:4:4 和 10 位 4:2:0，做不了 10 位 4:4:4），宿主的偏好也按组合排；以后的 4:2:2、HLG、12 位是新的值。列表和编码格式的 `codecs[]` 一样按偏好排、列出第二个就是接受替代、SDK 不加，放在 `rcs_encoder_config` / `rcs_decoder_config` 的 `codecs[]` 旁边，而不是另一个扩展：它们一起决定“发什么”，报价里每种编码格式带着画面格式的掩码。观看端必须列出才会收到：新格式改变交给渲染器的帧（P010、4:4:4 的布局、PQ 的值），渲染器不认识就会画错，这不能由分享端单方面决定。YCbCr 的矩阵和范围只由分享端选、不协商：每个解码器都能解，帧和流都写明它们，渲染器照着转换（以前的 `yuv_bt709` 就是这样）。CPU 帧仍是 BGRA8 SDR，HDR 不做色调映射，和 `on_frame` 一起时直接 `UNSUPPORTED`。HDR 元数据走流公告，不写进码流：随附的 NVENC 接口写不了，平台解码器也不一定交出 SEI，而宿主配置 HDR 交换链时要的就是这几个数。
33. **解码的“硬件优先”是加速方式的一个取值**：观看端要的是“能硬件就硬件，硬件解不了的格式用软件”（Windows 只有软件解码器解 HEVC 4:4:4），按每路流决定。做成 `rcs_acceleration` 的新值 `HARDWARE_FIRST`，而不是逐个格式指定加速方式的字段：宿主只表达一个意愿，SDK 按本机的实现列表和流的格式来选，结果由 `RCS_EVENT_CODEC` 报告。它是宿主明确的选择，解码器失败时不会换成另一个，所以不违反“不悄悄降级”。只用于解码器：软件编码器只有 H.264 4:2:0 8 位，补不上硬件做不了的格式；没有硬件编码器的机器（虚拟机）由宿主选 `SOFTWARE`。为了让用 GPU 渲染的宿主也能用它，Windows 的软件解码改为把画面上传成 BGRA8 纹理，而不是拒绝 `on_gpu_frame`。
34. **边缘功能是模块，官方的和别人的同一套接口**（见 [MODULES](MODULES.md)）：剪贴板、文件传输、消息这类功能不进核心，否则核心越长越胖，一个功能的代码散在会话、协议、助手链接、C 接口里，不用它的宿主也背着它。模块是一张函数表加一个 id，宿主注册、在扩展链里给它设置（`RCS_EXT_MODULE`，一条链里任意多项）才启用；没注册或没给设置的模块零线程、零消息。核心给模块的是宿主能看到的那些（通过 `rcs_module_session` 调公开函数），加上模块专用的几样：两端互报的模块清单和问候（只在两端都用、版本相同的连接上激活，没激活的原因报告给宿主）、按观看端的可靠和不可靠消息（各有队列、轮流发、`REPLACE`）、机器那一半（在助手里，在哪个进程都只通过消息联系，所以写一次两种情况都对）、事件（一种 `RCS_EVENT_MODULE`，`kind` 和 payload 由模块定义）、统计。官方模块只包含公开头文件，官方模块需要的能力就是第三方得到的能力，不开后门；模块碰不到内部对象，也不参与身份和握手。调用不重叠由 SDK 保证（模块状态不用为 SDK 的回调加锁），模块在调用里发的事件等调用返回才交给宿主，避免宿主在 `on_event` 里等模块、模块在等 `on_event` 的死锁。模块不能让会话失败；对端违反 SDK 的协议照旧失败连接。不做运行时从目录加载插件：信任谁、从哪里加载是宿主的决定。
35. **模块要的，加进模块接口；会话之外的，做成组件**：文本插入点要知道控制在谁手里、共享的那块在屏幕上的哪里，这两样做成所有模块都能用的 `peer_control` 回调和 `rcs_module_get_shared` 服务，而不是给插入点留后门；缩略图的拍摄是采集的一部分（Windows 一个进程只能复制一个显示器一次），留在核心里作为服务 `rcs_capture_picture`，模块只管到会话的助手那里去拍。看护者和会话列表不属于任何会话，不是模块，而是单独的库（组件，`components/`），也只用公开头文件；它们要的日志和失败原因，做成 `rcsdk.h` 里给所有建在 SDK 上的库用的 `rcs_log_write`、`rcs_log_in_callback`、`rcs_set_last_call_error`。被看护进程那一边（等结束、取密钥、请看护者发 Ctrl+Alt+Del）是会话自己要用的，留在核心。
36. **接口分三层，预设决定没放进链的扩展**：入门（角色、预设、怎么连、怎么证明身份，都在核心配置里）、功能（每个功能一个扩展，只放宿主的决定）、调优与替换（引擎参数合成三个调优扩展；宿主接管的一环各自一个头文件和扩展，回调放在它的扩展里、带自己的 `user_data`）。层次只是头文件和扩展的划分，代码都在核心里，不另起库。预设是“宿主没有放进链的扩展用哪套值”：放进了的完全是宿主的，不和预设逐字段合并，所以没有“这个字段谁说了算”的问题；要“某个预设、只改一个字段”就用 `rcs_preset_defaults` 拿到那个预设的整份扩展再改。预设只管画面、编码、声音、观看端的要求和三个调优扩展（第 6 节），共享什么、给不给控制、身份、服务器这些产品决定不随预设变。`*_INIT` 就是 `BALANCED`，不用预设的宿主得到的就是它。写死的超时（连接存活、握手、P2P 收集和检查、TURN 分配）开放进 `RCS_EXT_NETWORK_TUNING`；P2P 失败后改不改走中继仍是宿主的决定，SDK 不加“直连和中继一起试”的传输方式。
37. **保持唤醒是模块，模块能知道谁连着**：保持唤醒是产品策略（只在有人看时、还是整个运行；系统还是显示器），跑在助手那台机器上，按观看端来去开关，正好是模块的形状：会话那一半决定，机器那一半持有系统的电源请求，系统拒绝时报告（`RCS_POWER_CHANGED`、`rcs_power_get_status`），运行照旧。它要知道有没有观看端连着，而不管对方用不用这个模块，所以模块接口加了 `connected` / `disconnected` 回调（每个连接都来），而不是给它留后门。
38. **进程只有一份的东西用应用级的函数，按会话的用扩展；暂停有两个来源、各记各的**（见 [IOS-DESIGN](IOS-DESIGN.md)）：iOS 的 AVAudioSession 整个进程只有一个，按会话设置会互相覆盖，所以 `rcs_set_audio_session_config` 是应用级的，SDK 管时由一个协调者把所有会话要的合起来；宿主说自己管，SDK 就一点不碰、设得不对时报告原因，不悄悄改。观看端的暂停是宿主的动作（`rcs_session_pause` / `resume`，iPad 的多个窗口、宿主的流程），进后台的自动暂停是会话的设置（`RCS_EXT_BACKGROUND`，只录不看的宿主可以关掉）：两者各记各的，宿主的恢复不会取消系统要求的暂停，事件同时说明两者。暂停画面沿用“观看端不要这几路”这条现成的路（分享端停发、没人要的不采集），不另造一套。
39. **对方的状态单独报告；宿主推 GPU 图像是通用接口**：分享端也能暂停，观看端从 `RCS_EVENT_PAUSE` 的 `by_peer` 得知，和自己的两个来源分开记：观看端的恢复不会取消对方的暂停，宿主能区分“我暂停了”和“对方暂停了”（显示“对方已暂停”而不是黑屏）。`rcs_session_push_gpu_frame` 不是 iOS 专用的函数：它收 `rcs_gpu_frame_info`，本系统收哪种 GPU 资源写在能力里（`pushed_gpu_frames`），先实现 `CVPixelBuffer`，别的系统明确返回 `UNSUPPORTED`，以后加 D3D11 纹理只是多一种资源，不改函数。
40. **系统自己的图像交接各有一个函数，Java 经宿主交出的 VM**：Apple 的 `CMSampleBuffer` 和 Android 的 `AHardwareBuffer` 是宿主从系统拿到画面时的原样，各自一个函数（`rcs_session_push_sample_buffer`、`rcs_session_push_hardware_buffer`），都把任意尺寸的画面转正、放进流并报告位置，宿主不用自己缩放；不做一个收 `void*` 的通用函数，因为两种句柄的方向、生命周期和颜色约定都不同，错用时要能明确报 `UNSUPPORTED`。Android 上推进来的缓冲区一律先复制：BufferQueue 的生产者会重用宿主放手的缓冲区，不管谁还持有引用。SDK 不带 Java 代码：Android 只给 Java 的东西（权限、设备列表、剪贴板、唤醒锁）经宿主交出的 JavaVM 和 Context（`rcs_android_initialize`）去用，没交出时明确报告；模块经 `rcs_android_get_java` 用同一个，不各自去找 VM。
41. **一个 `rcs_session` 管两个角色**（考虑过拆开，不拆）：拆成分享端、观看端两种句柄，函数要多一倍（创建、启动、停止、事件、统计都要两份），C 绑定的负担更重；用错角色的函数返回 `UNSUPPORTED` 并说明，已经够清楚。头文件按角色分组，注释里写清每个函数和扩展给哪个角色。
42. **网络唤醒和远程电源操作是电源模块的，决定都是宿主的**（见[指南 15](guide/15-modules.md)）：睡着的机器不运行 SDK，SDK 只给积木（本机能唤醒它的网卡和唤醒设置、分享端在宿主打开时告诉观看端、从任何一端发魔术包），谁记住、由哪台设备、在哪个局域网里发是宿主的控制平面。远程重启 / 关机 / 睡眠 / 注销默认一个都不允许，宿主用 `actions` 允许、用 `on_action` 逐个决定（没有回调时只给有远程控制权的观看端）；做事的是机器那一半（有对应权限的助手），先答复、延迟后执行，做不到时说明原因而不换一种方式做（macOS 的 root 不会把不强制的重启变成强制的）；只报告系统和固件的唤醒设置，从不修改。系统调用藏在接缝后面，测试从不真的执行。

## 5. 以后的能力怎样加入

新能力放在哪一层，按下面的规则：

1. 一个字段属于哪一层，看一般宿主会不会、该不该改它：会改的是功能（`rcsdk.h` 的功能扩展，只放宿主的决定），“改了要先读性能文档”的是调优（`rcsdk/tuning.h`，预设给它们定值），宿主接管的一环是替换（各自一个头文件和扩展）。
2. 回调放在配置它的地方：入门和功能的回调在 `rcs_session_callbacks`，替换层的回调在它自己的扩展里，各自带 `user_data`。
3. 能做成模块的功能做成模块（`rcsdk/module.h`），不属于任何会话的做成组件（像看护者）。
4. 每层只依赖下层：入门不要求懂功能，功能不要求懂调优。
5. 加入的形式是新的扩展类型、事件、统计分区、函数或枚举值，不是核心配置的新字段；系统自己的类型走那个系统的头文件（`apple.h`、`android.h`）。

还没做、已经想好怎样加入的：

| 功能 | 配置 | 运行中 | 事件 | 统计 |
| --- | --- | --- | --- | --- |
| 网络路径变化 | — | — | 新事件 | `NETWORK` 新字段 |
| D3D12：不做。解码留在 D3D11 / Media Foundation 上；RTX 4090 上实测交给 D3D12 没有收益（见 [PERFORMANCE](PERFORMANCE.md)） | — | — | — | — |

这些能力的场景和优先级见 [ROADMAP](ROADMAP.md#通用化还缺的能力)。已经按这种方式加入的：日志、P2P、中继、动态码率、声音（两个方向：`SEND_AUDIO`、`RECEIVE_AUDIO`）、画质请求、单独传送的指针、多屏切换和多显示器（`STREAMS`）、窗口和区域采集（`CAPTURE.source` 的新值）、更多输入（`rcs_input_type` 的新值和 `input_types` 能力位；按类型交给宿主 `host_input_types`；文本插入点是模块 `rcsdk.text-caret`）、宿主自己的画面 / 声音 / 输入、常驻监听与重连、数据通道、身份方式、助手进程、无人值守（`secure_desktop`、`RCS_EVENT_DESKTOP`、`RCS_INPUT_SECURE_ATTENTION`、看护者组件）、停止通知、模块。

## 6. 预设的取值

`rcs_preset` 给下面这些扩展定值；表里没列的字段和没列的扩展都是 `*_INIT` 的值（`BALANCED`）。这些是建议值，还没在实机上逐个量过（量的方法和结果写进 [PERFORMANCE](PERFORMANCE.md)，量过以后可能调整）。代码里在 `src/session/config.cpp` 的 `presetSettings`。

| 扩展 | 字段 | `BALANCED`（`*_INIT`） | `LOW_LATENCY` | `LOW_BANDWIDTH` | `QUALITY` |
| --- | --- | --- | --- | --- | --- |
| `SHARE_VIDEO` | `width` × `height` | 1920×1080 | — | 1280×720 | — |
| | `frames_per_second` | 60 | — | 30 | — |
| | `bitrate_bps` | 20 Mbit/s | 40 Mbit/s | 4 Mbit/s | 50 Mbit/s |
| | `min_bitrate_bps` | 0（500 kbit/s） | — | 300 kbit/s | — |
| `ENCODER` | `formats[]` | 4:2:0 8 位 | — | — | 4:4:4 8 位、4:2:0 8 位（观看端也列了 4:4:4 才用，否则按列表用下一个） |
| `SEND_AUDIO` | `bitrate_bps` | 128 kbit/s | — | 96 kbit/s | — |
| `VIEW_REQUEST` | `width` × `height`、`frames_per_second`、`bitrate_bps`、`audio_bitrate_bps` | 都是 0（分享端定） | — | 1280×720、30、4 Mbit/s、96 kbit/s | — |
| `VIDEO_TUNING` | 动态码率的阈值 `overuse_queue_ms` / `severe_queue_ms` / `clear_queue_ms`、`overuse_loss_permille` / `clear_loss_permille`、`decrease_percent` | 30 / 60 / 10 ms、100 / 50 ‰、85 % | — | 20 / 40 / 8 ms、50 / 10 ‰、80 %（降得更早） | — |
| | `start_bitrate_bps` | 0（4 Mbit/s） | 10 Mbit/s | 1 Mbit/s | — |
| | `frame_spread_us` | 1000 | 500 | — | — |
| | `fec_mode`、`fec_percent`、`ref_invalidation` | `AUTO`、50、1 | — | — | — |
| | `input_boost_ms` | 0 | 100 | — | — |
| | `speed` | `FASTEST` | — | `BALANCED` | `BALANCED` |
| | `keyframe_min_interval_ms` | 500 | — | 1000 | 1000 |
| | `queue_frames`、`max_queue_age_ms` | 64、80 ms | —、40 ms | —、150 ms | — |
| `AUDIO_TUNING` | `max_delay_ms`、`buffer_margin_us` | 200 ms、2000 µs | 60 ms、1000 µs | 250 ms、10 000 µs | — |
| | `redundancy` | 1 | — | 0（码率留给画面） | — |
| `NETWORK_TUNING` | `idle_timeout_ms` | 5000 | — | 10 000 | — |

观看端的 `DECODER` 不随预设变：宿主的渲染器能处理哪些画面格式只有宿主知道，所以 `QUALITY` 的 4:4:4 要观看端的宿主自己在 `rcs_decoder_config.formats` 里列上。
