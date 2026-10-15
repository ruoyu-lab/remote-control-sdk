# 接口索引（0.1.1-beta）

按主题列出公开的函数、结构体和扩展，以及讲它们的页面。每个名字的完整说明在头文件里（注释是接口的正式说明）；这里不写行号，用名字在头文件里搜索。C++ 包装（`rcsdk/rcsdk.hpp` 和各个 `.hpp`）的对应名字见 [01](../01-concepts.md#最小的完整程序) 和各页的例子。

## 库和系统（`rcsdk/rcsdk.h`）

| 名字 | 用途 | 页面 |
| --- | --- | --- |
| `RCSDK_VERSION`、`rcs_version_number`、`rcs_version` | 头文件和加载的库的版本 | [21](../21-versions-distribution.md) |
| `rcs_result`、`rcs_result_message`、`rcs_get_last_call_error`、`rcs_set_last_call_error` | 结果码和原因 | [18](../18-errors.md) |
| `rcs_get_capabilities`（`rcs_capabilities`）、`rcs_list_codec_implementations`（`rcs_codec_implementation`） | 本机能做什么 | [19](../19-platforms.md)、[13](../13-tuning.md#实现的选择) |
| `rcs_list_displays`（`rcs_display_info`）、`rcs_list_windows`（`rcs_window_info`）、`rcs_list_audio_devices`（`rcs_audio_device_info`） | 显示器、窗口、声音设备 | [07](../07-sharing.md)、[12](../12-audio.md) |
| `rcs_list_networks`（`rcs_network_info`）、`rcs_notify_network_change` | 本机的网络、宿主先看到的网络变化 | [09](../09-transports.md#网络变化连接迁移) |
| `rcs_list_wifi`（`rcs_wifi_info`）、`RCS_EVENT_STALLS`（`rcs_stalls_event`） | 本机连着的 Wi-Fi（信道、AWDL）、路径的周期性断流 | [09](../09-transports.md#wi-fi-周期性卡顿awdl) |
| `rcs_screen_capture_permission`、`rcs_input_permission`、`rcs_microphone_permission`，`rcs_request_*_permission` | 系统权限 | [19](../19-platforms.md#系统权限) |
| `rcs_generate_connection_key`、`rcs_generate_pin`、`rcs_generate_device_key`、`rcs_device_public_key` | 密钥、PIN、设备密钥 | [10](../10-security.md) |
| `rcs_set_audio_session_config`、`rcs_get_audio_session_config`（`rcs_audio_session_config`） | iOS 的声音会话 | [05](../05-quickstart-ios.md#声音会话) |
| `rcs_set_log_callback`（`rcs_log_record`）、`rcs_log_write`、`rcs_log_in_callback` | 日志 | [14](../14-events-stats-logging.md#日志) |
| `rcs_preset_defaults` | 某个预设的整份扩展 | [13](../13-tuning.md#预设) |

## 会话（`rcsdk/rcsdk.h`）

| 名字 | 用途 | 页面 |
| --- | --- | --- |
| `rcs_session_config`（`rcs_connect`、`rcs_identity`）、`rcs_session_callbacks` | 核心配置和回调 | [01](../01-concepts.md)、[02](../02-threading.md) |
| `rcs_session_create`、`start`、`stop`、`stop_with_reason`、`destroy` | 生命周期 | [01](../01-concepts.md#生命周期和状态) |
| `rcs_session_update`、`get_settings`、`set_preset`、`set_identity` | 运行中修改 | [01](../01-concepts.md#运行中修改) |
| `rcs_session_get_state`、`get_failure`、`get_stream_info`、`get_peer_info`、`get_control_info`、`get_desktop_info` | 查询当前的值 | [14](../14-events-stats-logging.md#查询当前的值) |
| `rcs_session_list_displays`、`list_windows`、`list_viewers` | 分享端的列表 | [07](../07-sharing.md) |
| `rcs_session_get_stats`、`get_viewer_stats`、`get_stream_stats`、`get_encoding_stats`、`get_module_stats`（`rcs_*_stats`） | 统计 | [14](../14-events-stats-logging.md#统计) |
| `rcs_session_get_path_info`（`rcs_path_info`） | 连接现在的路径和备用路径 | [09](../09-transports.md#网络变化连接迁移) |
| `rcs_event`（`RCS_EVENT_*` 和各个 `rcs_*_event`） | 事件 | [14](../14-events-stats-logging.md) |
| `rcs_session_signal` | P2P / 中继的信令 | [09](../09-transports.md) |
| `rcs_session_send_input`（`rcs_input_event`） | 远程输入 | [11](../11-input.md) |
| `rcs_session_send_data`（`rcs_data_message`） | 数据通道 | [09](../09-transports.md#数据通道) |
| `rcs_session_request_keyframe`、`pause`、`resume` | 关键帧、暂停 | [08](../08-viewing.md#暂停)、[07](../07-sharing.md#暂停分享) |
| `rcs_session_push_video_frame`（`rcs_frame`）、`push_audio`（`rcs_audio_frame`） | 宿主的画面和声音 | [07](../07-sharing.md#宿主自己的画面和声音) |
| `rcs_session_disconnect_viewer`、`set_viewer_control`、`set_main_viewer`、`set_viewer_video`（`rcs_viewer_video`）、`admit_viewer`（`rcs_viewer_decision`） | 一对多、低档和准入 | [07](../07-sharing.md#一对多) |

## 扩展

| 扩展 | 头文件 | 页面 |
| --- | --- | --- |
| `RCS_EXT_SHARE_VIDEO`、`ENCODER`、`CAPTURE`、`STREAMS`、`CONTROL`、`VIEWERS`、`ENCODINGS` | `rcsdk.h` | [07](../07-sharing.md)、[11](../11-input.md) |
| `RCS_EXT_VIEW_REQUEST`、`DECODER` | `rcsdk.h` | [08](../08-viewing.md) |
| `RCS_EXT_SEND_AUDIO`、`RECEIVE_AUDIO` | `rcsdk.h` | [12](../12-audio.md) |
| `RCS_EXT_BACKGROUND`、`ICE_SERVERS`、`RECONNECT`、`DEVICE_IDENTITY`、`LOG`、`MODULE` | `rcsdk.h` | [05](../05-quickstart-ios.md)、[09](../09-transports.md)、[10](../10-security.md)、[14](../14-events-stats-logging.md)、[15](../15-modules.md) |
| `RCS_EXT_PATHS` | `rcsdk.h` | [09](../09-transports.md#网络变化连接迁移) |
| `RCS_EXT_FORWARDER`（`rcs_forwarder_link`：UDP、TCP、WebSocket、宿主的通道） | `rcsdk.h` | [09](../09-transports.md#经转发器rcs_transport_forwarder)、[24](../24-forwarder.md) |
| `RCS_EXT_VIDEO_TUNING`、`AUDIO_TUNING`、`NETWORK_TUNING` | `tuning.h` | [13](../13-tuning.md) |
| `RCS_EXT_HOST_ENCODER`、`HOST_DECODER`、`ENCODED_OUTPUT` | `host_codec.h` | [17](../17-replacing.md) |
| `RCS_EXT_RATE_DECIDER` | `rate_control.h` | [17](../17-replacing.md#宿主决定码率) |
| `RCS_EXT_HOST_TRANSPORT` | `host_transport.h` | [09](../09-transports.md#宿主自己的传输rcs_transport_host) |
| `RCS_EXT_GPU_DEVICE` | `gpu.h` | [04](../04-quickstart-windows.md#观看端用-d3d11-显示画面) |
| `RCS_EXT_AGENT_LINK` | `agent.h` | [16](../16-unattended.md) |

## 替换层和平台的函数

| 名字 | 头文件 | 页面 |
| --- | --- | --- |
| `rcs_gpu_frame_retain`、`release`、`get_info`（`rcs_gpu_frame_info`）、`wait`；`rcs_session_push_gpu_frame` | `gpu.h` | [08](../08-viewing.md)、[07](../07-sharing.md#宿主自己的画面和声音) |
| `rcs_encoder_output_push`、`rcs_session_push_encoded_video`、`rcs_session_video_ready`（`rcs_encoded_packet`、`rcs_encoded_audio`） | `host_codec.h` | [17](../17-replacing.md) |
| `rcs_session_transport_receive`、`rcs_session_transport_closed`、`rcs_session_transport_restarted`（都带路由号） | `host_transport.h` | [09](../09-transports.md#宿主自己的传输rcs_transport_host) |
| 转发器（单独的库）：`rcsfwd_create` / `start` / `stop` / `destroy`（`rcsfwd_config`）、`rcsfwd_sign_token`（`rcsfwd_grant`、`rcsfwd_secret`）、`rcsfwd_generate_identity`、`rcsfwd_public_key_sha256`、`rcsfwd_get_stats`、`get_room_stats`、`list_viewers`、`close_room`、`get_port`（按入口，`rcsfwd_entry`）、`get_last_error` | `rcsfwd.h` | [09](../09-transports.md#经转发器rcs_transport_forwarder)、[24](../24-forwarder.md)、[22](../22-examples-tools.md) |
| `rcs_session_get_agent_info`、`rcs_agent_create` / `start` / `stop` / `destroy` / `get_state` / `get_failure` / `wait`（`rcs_agent_config`、`rcs_agent_callbacks`）、`rcs_helper_wait_for_stop`、`rcs_helper_get_key` | `agent.h` | [16](../16-unattended.md) |
| `rcs_session_push_sample_buffer`（`rcs_sample_buffer_info`） | `apple.h` | [05](../05-quickstart-ios.md) |
| `rcs_android_initialize`、`rcs_android_get_java`、`rcs_session_push_hardware_buffer`（`rcs_hardware_buffer_info`） | `android.h` | [06](../06-quickstart-android.md) |

## 模块（`rcsdk/module.h` 和各个模块的头文件）

| 名字 | 页面 |
| --- | --- |
| `rcs_register_module`、`rcs_list_modules`、`rcs_module_config`、`RCS_EVENT_MODULE` | [15](../15-modules.md#用一个模块) |
| `rcs_capture_picture`（`rcs_picture`） | [07](../07-sharing.md#共享什么rcs_capture_config) |
| 写模块：`rcs_module_definition`、`rcs_module_send`、`set_hello`、`send_machine`、`get_shared`、`emit`、`session`、`find`、`run`、`set_state`、`state`、`log` | [MODULES](../../MODULES.md) |
| 剪贴板（`rcsdk/clipboard.h`）：`rcs_clipboard_module`、`rcs_clipboard_settings`、`rcs_clipboard_send`、`get_peer`、`get_stats`、`system_formats` | [15](../15-modules.md#剪贴板rcsdkclipboard) |
| 文件传输（`rcsdk/file_transfer.h`）：`rcs_file_transfer_module`、`rcs_file_transfer_settings`、`rcs_file_send`、`accept`、`reject`、`cancel`、`pause`、`get_transfer`、`get_items`、`get_peer`、`discard` | [15](../15-modules.md#文件传输rcsdkfile-transfer) |
| 电源（`rcsdk/power.h`）：`rcs_power_module`、`rcs_power_settings`、`rcs_power_get_status`、`rcs_power_get_capabilities`；网络唤醒 `rcs_power_list_interfaces`、`rcs_power_send_wake`、`rcs_power_get_peer`、`rcs_power_get_peer_interfaces`；远程电源操作 `rcs_power_request_action`、`RCS_POWER_ACTION` | [15](../15-modules.md#电源rcsdkpower) |
| 缩略图（`rcsdk/thumbnail.h`）：`rcs_thumbnail_module`、`rcs_thumbnail_capture` | [15](../15-modules.md#缩略图rcsdkthumbnail) |
| 文本插入点（`rcsdk/text_caret.h`）：`rcs_text_caret_module`、`rcs_text_caret_settings`、`rcs_text_caret_get_peer`、`rcs_text_caret_available` | [15](../15-modules.md#文本插入点rcsdktext-caret) |

## 组件

| 名字 | 页面 |
| --- | --- |
| 看护者（`rcsdk/supervisor.h`，Windows）：`rcs_supervisor_create`、`start`、`stop`、`destroy`、`get_status`、`update`（`rcs_supervisor_config`、`rcs_supervisor_event`）、`rcs_list_os_sessions`（`rcs_os_session_info`） | [16](../16-unattended.md#windows) |

## C++ 包装的辅助头文件

| 头文件 | 内容 |
| --- | --- |
| `rcsdk/rcsdk.hpp` | `rcsdk::SessionConfig`、`Session`、`Callbacks`、`GpuFrame`、`Error`、`defaults<T>()`、`capabilities()`、`displays()`、`windows()`、`generateConnectionKey()` 等 |
| `rcsdk/view_geometry.hpp` | 窗口坐标、编码帧和共享的显示器之间的换算（[08](../08-viewing.md#画面在窗口里的位置和坐标)） |
| `rcsdk/tuning.hpp`、`host_codec.hpp`、`rate_control.hpp`、`host_transport.hpp`、`gpu.hpp`、`agent.hpp`、`apple.hpp`、`android.hpp` | 各层的 `defaults<T>()`、接口类和组装函数 |
| `rcsdk/supervisor.hpp` | `rcsdk::Supervisor`、`supervisorConfig`、`osSessions` |
