# 默认值（0.1.4-beta）

每个结构体用它的 `*_INIT` 宏初始化，宏里就是下面这些值（也是预设 `BALANCED` 的值；其他预设改的字段见 [API-DESIGN 第 6 节](../../API-DESIGN.md#6-预设的取值)）。不放进扩展链的扩展取预设的值。以头文件为准：`include/rcsdk/*.h`、各模块的头文件。

## 核心配置

| 结构体（宏） | 字段 | 默认 |
| --- | --- | --- |
| `rcs_session_config`（`RCS_SESSION_CONFIG_INIT`） | `version` | `RCSDK_VERSION` |
| | `role` | `RCS_ROLE_VIEW` |
| | `preset` | `RCS_PRESET_BALANCED` |
| | `connect` | `RCS_TRANSPORT_UDP`、`"127.0.0.1"`、47990 |
| | `identity` | `RCS_SECRET_KEY`、`key` 全零（宿主必须填）、`password` 空、`pin_attempts` 5 |
| | `extensions` | `NULL` |
| `rcs_session_callbacks`（`RCS_SESSION_CALLBACKS_INIT`） | 全部回调、`user_data` | `NULL` |

## 功能扩展（`rcsdk/rcsdk.h`）

| 扩展（宏） | 默认 |
| --- | --- |
| `rcs_share_video_config`（`RCS_SHARE_VIDEO_CONFIG_INIT`） | 1920×1080、60 FPS、20 Mbit/s；`rate_mode` `RCS_RATE_DEFAULT`（动态）；`min_bitrate_bps` 0（500 kbit/s）；`follow_requests` 1；`max_width` / `max_height` 0（显示器的像素尺寸）、`max_frames_per_second` 0（显示器的刷新率，不知道时 60）、`max_bitrate_bps` 0（200 Mbit/s） |
| `rcs_encoder_config`（`RCS_ENCODER_CONFIG_INIT`） | `codecs` {H.264}；`formats` {`RCS_PICTURE_420_8`}；`RCS_MATRIX_BT709`、`RCS_RANGE_LIMITED`；`RCS_ACCELERATION_HARDWARE`；`RCS_BACKEND_AUTO` |
| `rcs_capture_config`（`RCS_CAPTURE_CONFIG_INIT`） | `RCS_CAPTURE_DISPLAY`、`display_id` 0（主显示器）、区域 / 窗口 / 进程 0、`exclude_current_process` 0、`secure_desktop` 0 |
| `rcs_streams_config`（`RCS_STREAMS_CONFIG_INIT`） | `displays` 全 0（只有第 0 路）、`weights` 全 0（按像素分码率） |
| `rcs_control_config`（`RCS_CONTROL_CONFIG_INIT`） | `allowed` 0、`RCS_INPUT_SYSTEM`、`host_input_types` 0、`caps_lock` `RCS_CAPS_LOCK_INPUT_SOURCE`、`caps_lock_hold_ms` 0（500 ms） |
| `rcs_viewers_config`（`RCS_VIEWERS_CONFIG_INIT`） | `max_viewers` 0（64）、`RCS_BITRATE_FROM_QUORUM`、`quorum_percent` 50、`admission_timeout_ms` 60 000 |
| `rcs_encodings_config`（`RCS_ENCODINGS_CONFIG_INIT`） | `RCS_ENCODINGS_AUTO`（需要时才开低档）、`count` 2；`lower[]` 的 `frames_per_second` 0（主档的一半，第二档四分之一）、`max_pixel_rate` 0（1920×1080×30）、`codecs` 全 0（本端的）、`RCS_SCALABILITY_L1T1`；主档 `scalability` `RCS_SCALABILITY_L1T1`（不分层） |
| `rcs_view_request_config`（`RCS_VIEW_REQUEST_CONFIG_INIT`） | 全 0（分享端决定）、`cursor` `RCS_CURSOR_IN_VIDEO` |
| `rcs_decoder_config`（`RCS_DECODER_CONFIG_INIT`） | `codecs` {H.264}；`formats` {`RCS_PICTURE_420_8`}；`RCS_ACCELERATION_HARDWARE`；`RCS_BACKEND_AUTO` |
| `rcs_send_audio_config`（`RCS_SEND_AUDIO_CONFIG_INIT`） | `enabled` 1；`codecs` {Opus, AAC, PCM}；128 000 bit/s；`RCS_AUDIO_SYSTEM`；`exclude_current_process` 0；`device` 0（系统默认）；`voice_processing` 1；`sample_buffer_audio_percent`、`sample_buffer_microphone_percent` 100（sample buffer 的两种声音都发）。不挂这个扩展时：分享端发、观看端不发 |
| `rcs_receive_audio_config`（`RCS_RECEIVE_AUDIO_CONFIG_INIT`） | `enabled` 1；`codecs` {Opus, AAC, PCM}；`playback` 1。不挂这个扩展时：观看端收、分享端不收 |
| `rcs_background_config`（`RCS_BACKGROUND_CONFIG_INIT`） | `pause` `RCS_PAUSE_VIDEO` |
| `rcs_ice_servers_config`（`RCS_ICE_SERVERS_CONFIG_INIT`） | 没有 STUN、没有 TURN；`RCS_TURN_UDP`；证书指纹全零（系统信任）；服务器名空 |
| `rcs_reconnect_config`（`RCS_RECONNECT_CONFIG_INIT`） | `keep_listening` 1、`reconnect` 1 |
| `rcs_forwarder_config`（`RCS_FORWARDER_CONFIG_INIT`） | 指纹全零、令牌空（`FORWARDER` 时都必填）；`link` `RCS_FORWARDER_UDP`，`websocket_url` 空，`tls_certificate_sha256` 全零（`wss://` 时按系统的信任和主机名核对）；`refresh_token` 没有；`key_rotation_ms` 0（只在有人离开时换代）；`from_admission` 0（新人从转发器留着的关键帧开始，看得到它来之前至多 `cache_ms` 的） |
| `rcsfwd_config`（`RCSFWD_CONFIG_INIT`，转发器） | 不听任何入口（至少填一个）；`max_rooms` 64、`max_viewers_per_room` 64、`max_ends` 1024；`max_viewer_bitrate_bps` 100 Mbit/s、`max_pipe_bps` 50 Mbit/s、`max_publish_bitrate_bps` 400 Mbit/s；`cache_ms` 3000、`cache_bytes` 16 MiB、`max_cache_bytes` 256 MiB（[24](../24-forwarder.md#配置文件)） |
| `rcs_paths_config`（`RCS_PATHS_CONFIG_INIT`） | `migrate` 1、`excluded_networks` 0（所有网络）、`RCS_PATH_POLICY_FOLLOW_SYSTEM`、`standby` 0、`restore_control` 0 |
| `rcs_device_identity_config`（`RCS_DEVICE_IDENTITY_CONFIG_INIT`） | 没有设备密钥、不限制对端（`trusted_key_count` 0） |
| `rcs_log_config`（`RCS_LOG_CONFIG_INIT`） | `max_level` 0（跟随 `rcs_set_log_callback` 的） |
| `rcs_module_config`（`RCS_MODULE_CONFIG_INIT`） | `module` `NULL`、`settings` `NULL`、`size` 0 |
| `rcs_audio_session_config`（`RCS_AUDIO_SESSION_CONFIG_INIT`，iOS） | `RCS_AUDIO_SESSION_SDK`、`mix_with_others` 0、`default_to_speaker` 1、`bluetooth_microphone` 1 |

## 调优扩展（`rcsdk/tuning.h`）

| 扩展（宏） | 字段 | 默认 |
| --- | --- | --- |
| `rcs_video_tuning_config`（`RCS_VIDEO_TUNING_CONFIG_INIT`） | `overuse_queue_ms` / `severe_queue_ms` / `clear_queue_ms` | 30 / 60 / 10 |
| | `overuse_persist_ms` | 150 |
| | `overuse_loss_permille` / `clear_loss_permille` | 100 / 50 |
| | `decrease_percent` / `decrease_interval_ms` | 85 / 300 |
| | `probe_increase_percent` / `increase_percent` / `recovery_increase_percent` | 50 / 8 / 25 |
| | `start_bitrate_bps` / `path_change_percent` | 0（4 Mbit/s）/ 50 |
| | `degradation` / `min_size_percent` | `RCS_DEGRADE_FRAME_RATE` / 50 |
| | `frame_spread_us` / `max_pacing_bps` | 1000 / 800 Mbit/s |
| | `path_pacing_percent` / `path_burst_us` | 110 / 8000 |
| | `fec_mode` / `fec_percent` / `ref_invalidation` | `RCS_FEC_AUTO` / 50 / 1 |
| | `input_boost_ms` | 0 |
| | `speed` | `RCS_ENCODER_FASTEST` |
| | `content` / `software_threads` | `RCS_CONTENT_AUTO`（显示器、区域、窗口、应用是 `DETAIL`，宿主的画面是 `MOTION`）/ 0（逻辑核的一半，最多 8） |
| | `keyframe_interval_ms` / `keyframe_min_interval_ms` | 0（只在需要时）/ 500 |
| | `viewer_keyframe_interval_ms` / `loss_keyframes_move` | 2000 / 3 |
| | `move_down_ms` / `move_up_ms` / `lower_idle_ms` | 2000 / 10 000 / 10 000 |
| | `queue_frames` / `max_queue_age_ms` | 64 / 80 |
| `rcs_audio_tuning_config`（`RCS_AUDIO_TUNING_CONFIG_INIT`） | `capture_backend` / `capture_period_frames` | `RCS_AUDIO_CAPTURE_AUTO` / 0（后端的默认：Core Audio 进程捕获 240 帧） |
| | `redundancy` | 1 |
| | `max_delay_ms` / `buffer_margin_us` | 200 / 2000 |
| | `playback_period_frames` / `playback_queue_frames` | 0（macOS 256 帧；Windows 系统混音周期）/ 0（一个半周期） |
| `rcs_network_tuning_config`（`RCS_NETWORK_TUNING_CONFIG_INIT`） | `local_interface` | 空（所有网卡） |
| | `wifi_low_latency` / `audio_dscp` | 1 / 0 |
| | `send_queue_bytes` / `in_flight_bytes` | 64 MiB / 64 KiB |
| | `reconnect_window_ms` / `reconnect_interval_ms` | 30 000 / 1000 |
| | `connect_timeout_ms` / `idle_timeout_ms` | 5000 / 5000 |
| | `ice_gather_ms` / `ice_connect_ms` / `turn_allocate_ms` | 1500 / 15 000 / 5000 |
| | `ice_addresses` | 0（只有网卡的地址） |
| | `path_stall_ms` / `path_hold_ms` / `standby_probe_ms` | 1000 / 30 000 / 15 000 |

## 替换层

| 扩展（宏） | 默认 |
| --- | --- |
| `rcs_host_encoder_config`（`RCS_HOST_ENCODER_CONFIG_INIT`，`rcsdk/host_codec.h`） | `codecs` 空（宿主必须填）、`picture_formats` `1u << RCS_PICTURE_420_8`、`cpu_images` 0、`RCS_ACCELERATION_HARDWARE`、`low_latency` 1、`temporal_layers` 1、`name` 空（必须填）、回调 `NULL` |
| `rcs_host_decoder_config`（`RCS_HOST_DECODER_CONFIG_INIT`） | `codecs` 空、`picture_formats` `1u << RCS_PICTURE_420_8`、`RCS_ACCELERATION_HARDWARE`、`name` 空、回调 `NULL` |
| `rcs_encoded_output_config`（`RCS_ENCODED_OUTPUT_CONFIG_INIT`） | `decode` 1、`encodings` 1（主档的包）、回调 `NULL` |
| `rcs_rate_decider_config`（`RCS_RATE_DECIDER_CONFIG_INIT`，`rcsdk/rate_control.h`） | 回调 `NULL` |
| `rcs_host_transport_config`（`RCS_HOST_TRANSPORT_CONFIG_INIT`，`rcsdk/host_transport.h`） | `RCS_HOST_CHANNEL_DATAGRAM`、`host_max_datagram` 0（1200）、回调 `NULL` |
| `rcs_gpu_device_config`（`RCS_GPU_DEVICE_CONFIG_INIT`，`rcsdk/gpu.h`） | `d3d11_device` `NULL` |
| `rcs_agent_link_config`（`RCS_AGENT_LINK_CONFIG_INIT`，`rcsdk/agent.h`） | `endpoint` 空（助手在本进程）、`key` 全零、`RCS_AGENT_ACCESS_PRIVILEGED`、`on_agent_attach` `NULL`（接受每个持有密钥的助手） |
| `rcs_agent_config`（`RCS_AGENT_CONFIG_INIT`） | `RCSDK_VERSION`、`RCS_AGENT_ACCESS_PRIVILEGED`、`reconnect` 1、`reconnect_interval_ms` 1000、`extensions` `NULL` |
| `rcs_supervisor_config`（`RCS_SUPERVISOR_CONFIG_INIT`，`rcsdk/supervisor.h`） | `RCSDK_VERSION`、命令行空、`RCS_HELPER_SYSTEM`、`os_session` 0（控制台）、`helper_key` 全零、`restart_delay_ms` 2000、`restart_delay_max_ms` 30 000、`stop_grace_ms` 3000 |

## 官方模块的设置

| 设置（宏） | 默认 |
| --- | --- |
| `rcs_clipboard_settings`（`RCS_CLIPBOARD_SETTINGS_INIT`） | `flow` 0（什么都不传）、`RCS_CLIPBOARD_SYSTEM`、`formats` 0（本机剪贴板能带的全部）、`max_bytes` 16 MiB、`check_interval_ms` 250、`send_existing` 0、`send_concealed` 0、`files_prefetch_bytes` 0（只在粘贴时取文件）、`files_directory` 空（系统临时目录里模块自己的）、`files_stand_in` 1（macOS 的 Finder 占位） |
| `rcs_file_transfer_settings`（`RCS_FILE_TRANSFER_SETTINGS_INIT`） | `flow` 0、`RCS_FILE_SYSTEM`、`max_items` 10 000、`max_offer_bytes` 0（不限）、`max_pending_offers` 100、`max_pending_items` 20 000、`max_pending_items_total` 100 000、`max_publications` 256、`max_fetches` 4、`offer_timeout_ms` 120 000、`reconnect_wait_ms` 60 000、`max_bytes_per_second` 0（不限）、`keep_partial` 1 |
| `rcs_power_settings`（`RCS_POWER_SETTINGS_INIT`） | `RCS_KEEP_AWAKE_SYSTEM`、`while_running` 0、`share_wake_info` 0、`actions` 0（不许远程电源操作）、`action_delay_ms` 3000、`on_action` `NULL`（设置为 `NULL` 时也是这样） |
| `rcs_text_caret_settings`（`RCS_TEXT_CARET_SETTINGS_INIT`） | `ask` 1、`interval_ms` 100 |
