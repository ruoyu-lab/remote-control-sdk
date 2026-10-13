# 14 事件、状态、统计与日志

SDK 内部策略的每次变化都能从事件和统计里看到：事件报告变化，查询函数返回当前值，统计按分区给计数和耗时，日志说明发生了什么和为什么。

## 事件：`on_event`

`on_event(const rcs_event* event, void* user_data)` 汇报各种变化（SDK 的线程，调用之间不重叠，见 [02](02-threading.md)）。`rcs_event` 的信封：`type`、`timestamp_ns`（本机单调时钟）、`viewer_id`（分享端：事件关于哪个观看端；观看端总是 0）、`stream_index`（`STREAM`、`CODEC`、`CAPTURE`、`RECOVERY`、`KEYFRAME`、`CURSOR` 事件关于哪一路画面），以及一个 union：和 `type` 同名的成员有效。事件结构只在回调期间有效。以后加的事件只增加 `type`，不需要新的回调：不认识的 `type` 跳过即可。

| 事件 | 成员 | 何时发出 |
| --- | --- | --- |
| `RCS_EVENT_STATE` | `state`（`value`、`result`） | 状态变化；`FAILED` 时 `result` 是错误码，`RECONNECTING` 和观看端离开后再次 `LISTENING` 时是连接结束（或路径没了）的原因（`DISCONNECTED`、`RESTARTING` 或 `NETWORK`）。观看端的连接留着时 `RECONNECTING` 之后可能直接回到 `RUNNING`，没有新的 `PEER`、`STREAM` |
| `RCS_EVENT_PEER` | `peer`（`rcs_peer_info`） | 连上对端（平台、提供的编码和声音格式、各格式的最大画面、是否开着声音、接受的输入类型、证明了的设备公钥），或对端运行中改了这些；分享端带 `viewer_id` |
| `RCS_EVENT_VIEWER` | `viewer`（`rcs_viewer_info`） | 分享端：一个观看端加入或离开（`joined`；离开时 `result`、`message` 是原因） |
| `RCS_EVENT_PIN` | `pin` | 分享端，`RCS_SECRET_PIN`：一次 PIN 握手失败了（`failures`、`attempts_left`；0 表示不再接受 PIN 握手，直到宿主重新设置身份） |
| `RCS_EVENT_ADMISSION` | `admission` | 宿主之后再决定的观看端（`RCS_ADMIT_LATER`）：`WAITING`、`ADMITTED`、`REFUSED`，分享端还有 `GONE`；两端都有 |
| `RCS_EVENT_STREAM` | `stream`（`changes`、`info`） | 画面开始或变了：`changes` 位说明编码格式、画面格式（含颜色和 HDR 元数据）、尺寸、帧率、码率、画面位置（黑边）、显示器、声音、指针模式变了，观看端被换到分享端的另一档编码或分享端开停了一档（`RCS_STREAM_ENCODING`），或这一路结束（`RCS_STREAM_ENDED`）；`info` 是当前的全部信息（分享端的是主编码） |
| `RCS_EVENT_CODEC` | `codec` | 编码器或解码器打开（方向、编码格式、加速方式、后端、实现名称，编码器能不能做参考帧失效 `ref_invalidation`），或编码器切换了模式（macOS 实时编码器切到流水线编码器：`low_latency` 和 `ref_invalidation` 变成 0） |
| `RCS_EVENT_CAPTURE` | `capture` | 分享端采集暂停或恢复，`paused` 是原因：`DESKTOP`、`HIDDEN`、`GONE`、`AGENT`；`NONE` 是恢复 |
| `RCS_EVENT_DISPLAYS` | `displays` | 分享端的显示器增减或变了模式（每 2 秒核对一次），`rcs_session_list_displays` 列出现在的 |
| `RCS_EVENT_RECOVERY` | `recovery` | 画面要重新开始：观看端积压被丢弃（`BACKLOG`）、解码器被替换（`DECODER`）、有一端要了关键帧（`REQUESTED`）；网络上丢了一帧补不回来（`LOSS`）：观看端在画面继续时报告，`method` 说从关键帧还是从“不再参考丢失帧”的普通帧继续，`frozen_ns` 是画面停了多久；分享端在继续的那一帧发出时报告 |
| `RCS_EVENT_KEYFRAME` | `keyframe` | 分享端、画面来源 `RCS_CAPTURE_ENCODED`：宿主的下一个包要是关键帧（流开始、有人要、到了间隔） |
| `RCS_EVENT_RATE` | `rate` | 分享端的动态码率调整了视频码率（新目标、丢包、到达速率、排队延迟、往返）；`viewer_id` 是码率卡在谁的路径上 |
| `RCS_EVENT_CURSOR` | `cursor` | 观看端、指针单独传送时：指针移动、换了形状或隐藏；宿主没有的形状（第一次出现，或已不在最近给的 32 张里）附带图像 |
| `RCS_EVENT_ENCODING` | `encoding` | 分享端：`stream_index` 这一路的一档低档编码（`RCS_EXT_ENCODINGS`）开了（`running` 1，格式、尺寸、帧率、码率）、停了（`running` 0、`RCS_OK`），或开不了 / 出错（`running` 0，`result`、`message`，如系统没有空余的编码会话）；主编码开始时仍是 `STREAM` 和 `CODEC`。`encoding` 为 0：运行中主编码要改（换编码格式、尺寸、帧率、画面格式），编码器却打不开（字段是想改成的样子，`result`、`message` 说为什么），流照旧按原来的跑（`running` 1，观看端看不到变化），同样的设置在变之前不再试；改的是共享什么（`rcs_capture_config`）而失败的，仍然结束运行 |
| `RCS_EVENT_VIEWER_VIDEO` | `viewer_video` | 分享端：`viewer_id` 换到另一档编码（`encoding`），原因 `reason`：`JOINED`（加入时就不在主编码）、`PATH`（路径装不下 / 又装得下）、`LOSS`（引起的关键帧太多）、`DECODER`（解不了）、`HOST`（`rcs_session_set_viewer_video`）、`ENCODING`（它那一档停了或开不了）；`target_bps` 是它的路径现在承受的码率 |
| `RCS_EVENT_AUDIO` | `audio` | 一个方向（`direction`）的声音开始、暂停或恢复：`active`、协商的格式、不工作时的原因（`result`、`message`，包括对方关了声音）、麦克风的语音处理是否在运行 |
| `RCS_EVENT_CONTROL` | `control`（`rcs_control_info`） | 远程控制被授予、暂停或撤销（`allowed`、`granted`） |
| `RCS_EVENT_DATA` | `data` | 数据通道的发送队列拒绝过一条消息之后，又有空间了（`queued_bytes`） |
| `RCS_EVENT_AGENT` | `agent`（`rcs_agent_info`） | 分享端：助手连上或离开，离开时 `result`、`message` 说明原因；助手在本进程时，运行开始时报告一次（[16](16-unattended.md)） |
| `RCS_EVENT_DESKTOP` | `desktop` | 机器显示的桌面变了：用户桌面或安全桌面（登录屏、锁屏、UAC），以及有没有用户登录。Windows 和 macOS 上有；分享端第一次观察到就报告，观看端连上时得到当前的 |
| `RCS_EVENT_MODULE` | `module` | 一个模块在某个连接上能用了（`RCS_MODULE_ACTIVE`）、不能用及原因（`INACTIVE`）、助手进程跑不了它的机器那一半（`NO_MACHINE`），或模块自己的事件（`kind` ≥ `RCS_MODULE_EVENT_OWN`，payload 由模块的头文件定义）（[15](15-modules.md)） |
| `RCS_EVENT_PAUSE` | `pause` | 两种角色：暂停的东西变了（`paused` 合起来，`by_host` 宿主的、`by_background` 进后台自动的、`by_peer` 观看端从分享端听到的）；一次运行开始时有暂停着的，先报告一次 |
| `RCS_EVENT_PATH` | `path`（`rcs_path_event`） | 连接的路径（[09 网络变化](09-transports.md#网络变化连接迁移)）：`CHANGING`（本端的网络变了而路径没回答，或路径 `path_stall_ms` 没有声音：视频、声音和数据停发）、`CHANGED`（换到了 `to`，`interruption_ns` 是中断多久，`signaled` 说这条路径是不是经宿主的信令找到的）、`LOST`（`idle_timeout_ms` 没有任何路径，或没有允许用的网络了：连接留着，`result`、`message` 说为什么）、`STANDBY`（备用路径好了，或没有及原因）；`cause` 是原因：本端的网络、路径沉默、对端换了地址（它先验证过 `PEER`，没验证过、多半是 NAT 重新映射 `REBINDING`）、设置、HOST 的通道换了。`from`、`to` 是 `rcs_path_info`：路线（直连、经 NAT、中继、宿主通道）、到中继走什么、本端的网络种类和标志（计费、省流量、默认、VPN）、网卡名、两端地址。分享端带 `viewer_id` |
| `RCS_EVENT_STALLS` | `stalls`（`rcs_stalls_event`） | 路径上的短断流（[09 Wi-Fi 周期性卡顿](09-transports.md#wi-fi-周期性卡顿awdl)）开始有稳定的节奏、节奏变了一成以上、或不再有节奏（`period_ms` 为 0）：`from_peer` 0 是本端收到的，1 是对端说它收到的（分享端带 `viewer_id`）；`period_ms` 节奏、`stall_ms` 中位长度、`stalls` 最近 10 秒几次；`path` 是本端当前的路径，本端走 Wi-Fi 时 `wifi` 是那个 Wi-Fi（`rcs_list_wifi` 的一项，否则名字为空）：`other_channel` 1 且节奏约 1050 ms，就是 AWDL 在抢网卡 |

一次连接里，`RCS_STATE_RUNNING` 在两端协商完成时报告，这个连接的其他事件（编解码器、画面、声音、控制权）都在它之后；对端的信息（`RCS_EVENT_PEER`）在连上对端、以及对端改了它提供的东西时报告。除此之外不要依赖事件的精确先后，按事件的内容处理。

## 查询当前的值

| 函数 | 返回 |
| --- | --- |
| `rcs_session_get_state` | 现在的状态 |
| `rcs_session_get_failure` | 最近一次 `start` 或运行为什么失败：结果码和说明（没有失败时 `RCS_OK`） |
| `rcs_session_get_stream_info(session, stream, &info)` | 一路画面最近一次公告的样子（`rcs_stream_info`）；还没开始的读出 `RCS_STREAM_INFO_INIT` 加上它的编号 |
| `rcs_session_get_peer_info` | 对端（分享端：主观看端）的 `rcs_peer_info` |
| `rcs_session_get_control_info` | 远程控制（分享端：主观看端的，没有观看端时是新来者会得到的） |
| `rcs_session_get_desktop_info` | 被共享的机器现在显示什么桌面 |
| `rcs_session_get_settings(session, &ext.header)` | 一个扩展现在实际用的设置（预设给的也一样） |
| `rcs_session_list_displays` / `rcs_session_list_windows` | 分享端：会话采集的机器的显示器 / 窗口 |
| `rcs_session_list_viewers` | 分享端：现在连着的观看端（按加入顺序） |
| `rcs_session_get_path_info(session, viewer, &active, &standby)` | 连接现在的路径和备用路径（`rcs_path_info`；没有时 `route` 为 `NONE`）：观看端传 0，分享端传观看端编号（0：主观看端） |
| `rcs_list_networks` | 本机现在的网络（默认网络在前）：名字、网卡编号、种类、标志、有没有 IPv4 / IPv6；不属于哪个会话 |
| `rcs_session_get_agent_info` | 分享端：助手的机器（`rcsdk/agent.h`） |

列表函数都分两遍：传 `NULL` 和 0 得到个数，再传够大的数组；容量不够时 `BUFFER_TOO_SMALL`。`item_size` 是元素类型的 `sizeof`。C++ 包装的 `rcsdk::displays()`、`session.viewers()` 等已经处理好了。

## 统计

`rcs_session_get_stats(session, section, &stats)` 按分区返回统计：每个分区有自己的结构体，用对应的 `*_STATS_INIT` 初始化；C++ 用 `session.stats<T>()`。计数在每次运行中累计，耗时是最近一项的值（纳秒，本机时钟）。请求本角色没有的分区返回 `UNSUPPORTED`，不存在的分区返回 `INVALID_ARGUMENT`。

| 分区 | 角色 | 内容 |
| --- | --- | --- |
| `RCS_STATS_CAPTURE`（`rcs_capture_stats`） | 分享 | 是否暂停及原因、采集的帧数、编码前被覆盖的帧数、暂停次数 |
| `RCS_STATS_VIDEO_SEND`（`rcs_video_send_stats`） | 分享 | 编码器实现名、是否低延迟模式、当前目标码率、编码帧数、关键帧数、参考帧失效后编出的帧数、字节、本地预览帧数、编码耗时、采集到出包的耗时 |
| `RCS_STATS_AUDIO_SEND`（`rcs_audio_send_stats`） | 两端 | 本端发出的声音：是否在传、格式、编码器名、占用码率（带冗余时是两倍）、采集后端和周期、采集的采样帧、编码包数与字节、暂停次数、编码耗时 |
| `RCS_STATS_VIDEO_RECEIVE`（`rcs_video_receive_stats`） | 观看 | 解码器实现名、收到 / 排队 / 丢弃的包、恢复请求、解码器重置与替换次数、解码与交付帧数、排队 / 解码 / 流水线耗时 |
| `RCS_STATS_AUDIO_RECEIVE`（`rcs_audio_receive_stats`） | 两端 | 本端收到的声音：是否在播、格式、解码器名、播放设备、输出周期和设备队列、当前缓冲与目标缓冲、收到 / 丢失 / 冗余补回的包、补出与丢弃的毫秒数、欠载（缓冲空了的 `underruns` 和设备自己空播的 `device_underruns`）、已播放的采样帧 |
| `RCS_STATS_FORWARDER`（`rcs_forwarder_stats`） | 两端，经转发器时 | 观看端在房间里的编号；流密钥的代和步（分享端：正在用的；观看端：最近打开画面的）；分享端换代次数、补发密钥次数；分享端封装并上传的、观看端打开的画面数和字节，声音包数（`sound_packets`）；观看端丢掉的画面和声音：验证不过或头和内容不一致（`media_rejected`）、重复（`media_replayed`）、没有密钥（`media_unkeyed`）；到转发器那一跳的往返和字节；怎样连的（`link`）；分享端：上行量到的码率（`uplink_bps`）和它限住编码的轮数（`uplink_limited`） |
| `RCS_STATS_NETWORK`（`rcs_network_stats`） | 两端 | 路径描述、往返时间、收发字节；当前路径的路线、本端走的网络和它的标志、路径是不是没了（`path_lost`）、备用路径有没有和它的网络、本次运行换了几次路径（`path_changes`，迁移不加 `connections`）和最近一次的中断、经计费网络收发的字节（`metered_bytes`，含备用路径的探测；按统计发布的节奏记，一秒几次）；分享端动态码率时的到达速率、丢包、排队延迟、决定过的报告数和宿主改过的数；分享端：观看端量出的路径速率（视频的发送节奏跟着它）；丢包修复（冗余比例、发出的冗余片、靠冗余补回的、再要一次补回的、放弃的帧）；标了 DSCP 的声音包数；声音在本端等了多久才发出、收到的声音在路径上排了多久（最近一到两秒的最大值）；Wi-Fi 是否保持不省电；路径的短断流（对端一直在发、本端 30 ms 以上什么都没收到：最近 10 秒几次、中位和最长的长度、有稳定节奏时的周期 `stall_period_ms`；对端说它看到的周期和中位长度 `peer_stall_*`，见 `RCS_EVENT_STALLS`）；本次运行通过握手的连接数、监听期间丢弃的连接、PIN 失败次数；观看端最近一个包的接收与解密耗时；HOST 上通道没收下的数据报数 |
| `RCS_STATS_INPUT`（`rcs_input_stats`） | 两端 | 分享端收到 / 执行的输入和系统最近的结果；观看端发出的输入、排队与发送耗时 |
| `RCS_STATS_DATA`（`rcs_data_stats`） | 两端 | 数据通道：排队字节、发出和收到的消息数与字节数 |
| `RCS_STATS_GROUP`（`rcs_group_stats`） | 分享 | 观看端数、每档几个人、第 0 路跑着几档、主编码的码率、最近一秒所有观看端连接的流量；关键帧按原因计数（加入、换档、丢帧或积压、宿主或间隔）、上移和下移次数、系统没给编码会话的次数、判为上行受限的轮数 |
| `RCS_STATS_VIEWER_VIDEO`（`rcs_viewer_video_stats`） | 分享，一个观看端的 | 它在哪一档、是否参与码率、路径的目标码率、交给连接的帧和它那一档里没拿到的帧、它引起的关键帧和等着的请求、上下移次数（用 `rcs_session_get_viewer_stats`） |

- **分享端有几个观看端时**：`NETWORK`、`AUDIO_SEND`、`AUDIO_RECEIVE`、`DATA`、`INPUT` 属于每个观看端的连接，`rcs_session_get_stats` 给的是所有观看端（含已离开的）计数的合计，测量值是主观看端的；一个观看端的用 `rcs_session_get_viewer_stats(session, viewer, section, &stats)`（离开的观看端保留最后 256 个）。
- **几路画面时**：`CAPTURE`、`VIDEO_SEND`、`VIDEO_RECEIVE` 属于每一路，`rcs_session_get_stats` 给第 0 路的，`rcs_session_get_stream_stats(session, stream, section, &stats)` 给每一路的。
- **几档编码时**（`RCS_EXT_ENCODINGS`）：`VIDEO_SEND` 是主编码的，`rcs_session_get_encoding_stats(session, stream, encoding, &stats)` 给每一档的（没在跑的档是 `INVALID_STATE`）。
- **模块的消息**：`rcs_session_get_module_stats(session, viewer, id, &stats)`（收发的消息数和字节、排队的字节）；模块自己的计数由它的函数给（剪贴板的 `rcs_clipboard_get_stats`）。

```cpp
const auto video = session.stats<rcs_video_receive_stats>();
const auto net = session.stats<rcs_network_stats>();
std::printf("%s：解码 %llu 帧，往返 %.1f ms，路径 %s\n", video.decoder,
            (unsigned long long)video.decoded_frames, net.round_trip_ns / 1e6, net.path);
```

`rcsdk_cli --report FILE` 把运行结束时的全部统计写成 JSON，可以看到每个字段的实际值（[22](22-examples-tools.md)）。

## 日志

`rcs_set_log_callback(max_level, callback, user_data)` 是进程级设置，把 SDK 内部发生的事交给宿主自己的日志系统。默认关闭；C++ 用 `rcsdk::setLogCallback`。

- **每条是一条记录**（`rcs_log_record`）：级别（`RCS_LOG_ERROR`、`WARNING`、`INFO`、`DEBUG`）、模块（`session`、`network`、`encoder`……）、内容（UTF-8）、时间戳（和事件同一个单调时钟），以及它属于谁：`source` 是 `SESSION`、`AGENT`、`SUPERVISOR` 或 `PROCESS`，对应的句柄，和那个对象回调表里的 `user_data`。一个进程里有几个会话时，宿主按 `user_data`（通常就是自己的通话、工单或设备对象）把记录归到对应的地方，不用另建映射。
- **归属**：会话自己的工作都记在会话名下（它的网络、声音、编解码线程，没有另一个进程的助手时它自己的采集和输入）；另一个进程里的助手在那个进程里记为 `AGENT`；看护者记为 `SUPERVISOR`；不属于任何对象的（能力查询、编解码实现列表）是 `PROCESS`。对象的 `destroy` 返回后，不会再有记录带着它的句柄。
- **级别**：会话设了 `RCS_EXT_LOG`（`rcs_log_config.max_level` 不为 0）就按它自己的，否则按 `rcs_set_log_callback` 的 `max_level`。所以可以只让排查中的那个会话输出 `DEBUG`；运行中用 `rcs_session_update` 改，立即生效。助手在 `rcs_agent_config.extensions` 里放同一个扩展（C++ `AgentConfig::setLogLevel`）。
- **内容**：会话开始与结束、协商结果、打开的编解码器、采集暂停、积压恢复、被丢弃的连接及原因、失败原因（包括系统错误）。不按帧记录，开着也不影响性能。
- **回调规则**：在 SDK 线程上逐条调用、不会重叠，要快；记录和其中的字符串只在回调期间有效。设置函数返回后，旧回调不会再被调用。在日志回调里调用设置函数，或者任何对象的 start / stop / destroy（以及 `rcs_agent_wait`），返回 `CALLBACK_THREAD`。
- 建在 SDK 上的库（宿主自己的模块、组件）用 `rcs_log_write` 写进同一个回调，用 `rcs_log_in_callback` 判断是不是在日志回调的线程上。

```cpp
rcsdk::setLogCallback(RCS_LOG_INFO, [](const rcs_log_record& r) {
    // 用 C 接口创建的会话：r.user_data 是它回调表的 user_data（宿主自己的通话、工单对象）；PROCESS 时是 NULL
    myLogger.write(r.source, r.user_data, r.level, r.module, r.message);
});
```

C++ 包装的 `Session` 和 `Agent` 自己用着 `user_data`，按句柄认：`r.session == session.handle()`。
