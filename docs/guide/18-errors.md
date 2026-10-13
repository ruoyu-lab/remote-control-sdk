# 18 错误处理

SDK 不悄悄降级：做不到就返回明确的结果码和原因，由宿主决定下一步。本页说结果码从哪里来、各是什么意思、宿主该怎么办。

## 结果从哪里来

| 来源 | 怎么拿 |
| --- | --- |
| 函数的返回值 | 每个动作函数返回 `rcs_result`（`int32_t`，`RCS_OK` 是 0，失败是负数） |
| 调用为什么被拒绝 | `rcs_get_last_call_error(buffer, capacity)`：**这个线程上**上一次 SDK 调用失败的原因（例如哪个扩展的哪个字段越界），失败后马上读。建在 SDK 上的库（模块、组件）的函数也这样报原因 |
| 启动或运行为什么失败 | `rcs_session_get_failure(session, &result, message, capacity)`：最近一次 `start` 或运行失败的结果码和说明；运行失败时还有状态事件 `RCS_STATE_FAILED` 和它的 `result` |
| 事件里的结果 | 状态事件的 `state.result`（`FAILED` 的原因；`RECONNECTING`、再次 `LISTENING` 时连接结束的原因）、`audio.result`（这个方向为什么不工作）、`agent.result`（助手为什么离开）、`viewer.result`（观看端为什么离开）、`admission.result`、`module.result`，以及模块自己的（文件传输的 `RCS_FILE_DONE`、保持唤醒的 `rcs_power_status`） |
| 统计里的结果 | `rcs_input_stats.last_result`：系统对最近一个远程输入的答复 |
| 助手、看护者 | `rcs_agent_get_failure`、助手的状态事件；`rcs_supervisor_event.result` |

- `rcs_result_message(result)` 给出结果码的英文短句；原因文字（`message`、`rcs_get_last_call_error`）也是英文，给开发者和日志看。给用户看的文字由宿主按结果码自己写（也方便本地化）。
- C++ 包装：构造会话、设置配置、产生新东西的函数失败时抛 `rcsdk::Error`（`result()` 是结果码，`what()` 带着原因）；动作返回 `rcs_result`；状态和信息查询是 `noexcept`；`session.failure()` 返回 `SessionFailure{result, message}`。

## 结果码

| 结果码 | 含义 | 常见的情况 | 宿主怎么办 |
| --- | --- | --- | --- |
| `RCS_OK`（0） | 成功 | | |
| `RCS_ERROR_INVALID_ARGUMENT`（-1） | 本版本没有定义的值，或越界；原因说是哪个 | 扩展字段越界；没用 `*_INIT` 初始化（`struct_size` 不对）；头文件和库不是同一个版本；同一种扩展放了两次；扩展属于另一个角色；`on_frame` 和 `on_gpu_frame` 都注册了；`rcs_session_signal` 收到的不是信令 | 是代码的错误：看 `rcs_get_last_call_error`，改代码 |
| `RCS_ERROR_INVALID_STATE`（-2） | 对象现在的状态做不了 | 没在运行时 `send_input`、`send_data`；分享端还没授权；宿主推画面时还没有流；运行中改在一次运行中固定的扩展；对已经决定过的观看端 `admit_viewer`；共享窗口时点在共享的东西外面（输入统计里） | 多半是暂时的：等对应的事件（`RUNNING`、`RCS_EVENT_CONTROL`、`RCS_EVENT_STREAM`）再做 |
| `RCS_ERROR_UNSUPPORTED`（-3） | 这台机器（或这个角色）做不到；原因说缺了什么 | 没有硬件编解码器（虚拟机、模拟器）；两端没有共同的编码格式、画面格式或声音格式；系统没有的功能（Windows 上 `push_gpu_frame`、非 Apple 系统上 `push_sample_buffer`、iOS 上 `SYSTEM` 声音）；对端的协议版本不同 | 宿主明确选另一条路：软件编解码、多列一个格式、改走中继、换设置；或者告诉用户这台设备做不到 |
| `RCS_ERROR_CALLBACK_THREAD`（-4） | 在对象自己的回调里调用了 start / stop / destroy（或在日志回调里调用了不能调的函数） | 收到某个事件就想停止会话 | 把这个调用派发到自己的线程（[02](02-threading.md)） |
| `RCS_ERROR_WRONG_THREAD`（-5） | 必须在应用的主线程上调用 | macOS 上弹权限提示 | 在主线程上调 |
| `RCS_ERROR_BUFFER_TOO_SMALL`（-6） | 列表或文字的缓冲区不够 | 列表函数的第二遍容量不够（窗口、会话列表随时变化）；缩略图的缓冲区不够 | 按返回的个数 / 大小重新分配再调（C++ 包装已处理） |
| `RCS_ERROR_QUEUE_FULL`（-7） | 发送队列现在放不下；之后会说什么时候可以 | `send_data` 队列满；`send_input` 队列满（已换成释放全部按键）；模块消息队列满；画面来源 `ENCODED` 时 `video_ready` 说路径还压着 | 等 `RCS_EVENT_DATA`（模块：`room`）再发；输入：停止捕获输入；码流：这一帧不编 |
| `RCS_ERROR_TIMEOUT`（-8） | 等不到 | 准入时宿主没在 `admission_timeout_ms` 内决定；问另一个进程的助手（窗口列表、缩略图）没答复；`rcs_agent_wait`、`rcs_gpu_frame_wait` 超时 | 视情况重试，或告诉用户对方没有回应 |
| `RCS_ERROR_CANCELLED`（-9） | 被取消或拒绝 | 文件传输被对方拒绝或取消 | 告诉用户 |
| `RCS_ERROR_INTERNAL`（-10） | SDK 内部或系统的意外 | 系统拒绝了电源请求；不该发生的错误 | 写日志（打开 `DEBUG`），报告问题 |
| `RCS_ERROR_PERMISSION_REQUIRED`（-11） | 缺系统权限 | 屏幕录制、辅助功能、麦克风没有授权；Ctrl+Alt+Del 的系统策略没设；看护者不是 LocalSystem；macOS 的助手没有屏幕录制授权 | 引导用户授权（`rcs_request_*_permission`），或改用不需要权限的方式（宿主执行输入、宿主推画面） |
| `RCS_ERROR_CAPTURE`（-12） | 屏幕采集失败 | 共享的显示器断开超过 10 秒；Windows 上隐藏不了本应用的窗口；同一个进程里两次运行复制同一个显示器；远程桌面会话里复制失败；开始采集时桌面已经不可捕获 | 让用户换一个要共享的东西，或重新开始一次运行 |
| `RCS_ERROR_CODEC`（-13） | 编解码器失败 | 编码器、解码器打不开某个尺寸；解码器恢复后又失败；宿主的编解码器返回了未定义的结果；编码器悄悄降低了格式（每个关键帧都核对） | 降低尺寸或换编码格式后重新开始；看原因里的实现名 |
| `RCS_ERROR_DEVICE`（-14） | 系统资源缺失、被占用或失败 | 没有声音设备或被拔掉（声音暂停，不结束运行）；剪贴板被别的程序占着；文件传输时磁盘空间不够；Android 上 GPU 画不了推来的缓冲区 | 多数会自己恢复；提示用户检查设备 |
| `RCS_ERROR_NETWORK`（-15） | 到对端的路径出了问题（经转发器时也包括：转发器不回答、房间里没有分享端） | `idle_timeout_ms` 没收到数据（迁移的连接：路径没了，`RCS_PATH_LOST`，连接先留着）；留着的时间用完；对端说已经没有这个连接；套接字错误；P2P 连通检查找不到路径；TURN 服务器不回答；重连时间窗用完；宿主调用了 `transport_closed`；`rcs_paths_config` 不许用的网络是唯一的网络 | 观看端会按设置自己重连；P2P 失败时宿主决定是否改走中继；第一次就连不上要检查地址和防火墙 |
| `RCS_ERROR_AUTHENTICATION`（-16） | 身份没有通过 | 密钥、口令或 PIN 不对（“Wrong PIN or another secret”）；PIN 失败次数到了（“The sharer accepts no more PIN attempts”）；分享端不信任本端的设备（“The sharer refused this end: …”）；对方的设备公钥不在信任列表里；TURN 凭据被拒；TURN 服务器的证书没通过；会话拒绝了这个助手 | 让用户重新输入；换 PIN；配对设备；更新凭据或证书指纹。不会自动重连 |
| `RCS_ERROR_PROTOCOL`（-17） | 对端发来了本版本不接受的东西 | 对端有问题或不是同一个版本的 SDK | 不会自动重连；检查两端的版本 |
| `RCS_ERROR_DISCONNECTED`（-18） | 对端关闭了连接（它停止了，或进程结束了） | 观看端：分享端停了；分享端：观看端走了 | 观看端不重连：告诉用户对方结束了共享 |
| `RCS_ERROR_RESTARTING`（-19） | 对端关闭了连接，并说会回来（`RCS_STOP_RESTART`） | 对方的程序在更新、助手在换会话 | 观看端自己立即重连，宿主显示“正在重新连接” |
| `RCS_ERROR_BUSY`（-20） | 分享端已经服务满了，或不能在服务别人的同时服务这一个；原因说是哪种 | `max_viewers` 满了；解不了正在传的格式或尺寸；没有共同的声音格式；文件传输等决定的清单满了 | 稍后再试，或告诉用户满员；重连中的观看端会在时间窗内自己重试 |
| `RCS_ERROR_REPLACED`（-21） | 另一个助手接替了这个 | 新的助手进程被会话接受 | 不再启动这个旧助手 |
| `RCS_ERROR_REFUSED`（-22） | 分享端的宿主没让这个观看端进来；`message` 是它的原因 | `on_viewer_join` 拒绝，或 `rcs_session_admit_viewer` 拒绝 | 告诉用户对方拒绝了；不自动重连 |

## 不悄悄降级时，宿主怎样明确地退一步

SDK 只在宿主明确要求时换一条路。常见的几种写法：

```cpp
// 1. 没有硬件解码器（虚拟机、模拟器）：改用软件解码，或者一开始就“硬件优先”
if (viewer.start() != RCS_OK && viewer.failure().result == RCS_ERROR_UNSUPPORTED) {
    // viewer.failure().message 写明缺了什么
    config.setAcceleration(RCS_ACCELERATION_HARDWARE_FIRST);   // 或 RCS_ACCELERATION_SOFTWARE
}

// 2. 两端没有共同的编码格式：多列一个替代（列出就是接受）
config.setCodecs({RCS_CODEC_HEVC, RCS_CODEC_H264});

// 3. P2P 打不通：两端都改走中继，重新开始
if (session.failure().result == RCS_ERROR_NETWORK && transport == RCS_TRANSPORT_P2P) {
    config.setRelay("turn.example.com:3478", turnUser, turnPassword);
}

// 4. 没有输入权限：先引导用户授权，或者只看不控
if (rcsdk::inputPermission() != RCS_PERMISSION_GRANTED) {
    rcsdk::requestInputPermission();   // macOS：主线程；用户授权后再允许控制
}
```

```c
/* C：调用被拒绝时读原因 */
if (rcs_session_update(session, &capture.header) != RCS_OK) {
    char reason[512];
    rcs_get_last_call_error(reason, sizeof reason);   /* 同一个线程、马上读 */
    log_error("update: %s", reason);
}
```
