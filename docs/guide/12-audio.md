# 12 声音

声音有两个方向：分享端机器的声音（或宿主推的）到观看端；观看端的麦克风（或宿主推的）到分享端，例如远程协助时帮忙的人一边操作一边说话。每一端用 `RCS_EXT_SEND_AUDIO`（`rcs_send_audio_config`）配置自己发出的声音，用 `RCS_EXT_RECEIVE_AUDIO`（`rcs_receive_audio_config`）配置自己收到的声音，两种角色都能用。

- **默认**（不挂扩展）：分享端发出本机正在播放的声音、不收；观看端收到并播放、不发。所以开箱即用就有分享端到观看端的声音。挂上 `RCS_SEND_AUDIO_CONFIG_INIT` / `RCS_RECEIVE_AUDIO_CONFIG_INIT` 就是打开（它们的 `enabled` 是 1）；C++ 的 `SessionConfig` 给观看端的发送、分享端的接收建扩展时保持关，用 `setSendAudio`、`setReceiveAudio`、`setMicrophone` 打开。
- **一个方向在发送方发、接收方收时才传**，两个方向互不影响：各自协商格式，各有自己的事件（`RCS_EVENT_AUDIO` 的 `direction`：`RCS_AUDIO_SENDING` / `RCS_AUDIO_RECEIVING`）和统计（`RCS_STATS_AUDIO_SEND` / `RCS_STATS_AUDIO_RECEIVE`，两种角色都有；分享端按观看端的用 `rcs_session_get_viewer_stats`）。对端发不发、收不收、用什么格式，见 `rcs_peer_info` 的 `sends_audio`、`receives_audio`、`send_audio_codecs`、`receive_audio_codecs`。
- **声音从不让画面停下**：没有设备、设备被拔掉、系统太旧、做不到排除本应用、没有麦克风权限时，这个方向暂停，`RCS_EVENT_AUDIO` 说明原因（`result`、`message`，例如 `RCS_ERROR_DEVICE`），条件恢复后自动恢复。一端关了这个方向时另一端收到 `active = 0`、`result = RCS_OK` 的事件（不算失败）。一个方向两端都开着、格式却没有交集时，和视频一样两端都以 `UNSUPPORTED` 失败。

## 编码格式

都是 48 kHz 立体声。每个方向像视频一样协商，用发送方列表里第一个接收方也列了的格式；分享端到观看端这个方向，观看端可以用 `rcs_view_request_config.audio_codec` / `audio_bitrate_bps`（C++ `setAudioRequest`）要求两端都有的另一个格式和码率，分享端按 `follow_requests` 决定是否照办。

| 格式 | 每包 | 编码延迟 | 说明 |
| --- | --- | --- | --- |
| `RCS_AUDIO_OPUS`（默认） | 跟着采集：macOS 的 Core Audio 进程捕获默认 5 ms，ScreenCaptureKit 20 ms，Windows 10 ms，宿主推的按推的长度（2.5～20 ms） | 约 2.5 ms | libopus（编进 SDK），丢包时由解码器补出近似的声音；码率 16～510 kbit/s，一包最多 1024 字节，所以 20 ms 一包时最多约 400 kbit/s |
| `RCS_AUDIO_AAC` | 约 21 ms | 约 40～60 ms | 系统的编解码器（AudioToolbox、Media Foundation、MediaCodec），AAC-LC 恒定码率 64～320 kbit/s；Windows 和 Android 上只能是 96、128、160、192 kbit/s，取最接近 `bitrate_bps` 的一档（发送统计的 `bitrate_bps` 是实际用的） |
| `RCS_AUDIO_PCM` | 5 ms | 0 | 不压缩，16 位，1536 kbit/s，适合局域网 |

每一端只提供本机能编码（发）或解码（收）的格式：能力里的 `audio_encoders` / `audio_decoders`（`1u << rcs_audio_codec` 位）。Opus 和 PCM 始终有；AAC 依赖系统（没装 Media Foundation 的 Windows、Linux 没有）。开着的方向的 `codecs[]` 里一个本机有的格式都没有时，`rcs_session_start`（和运行中的 `rcs_session_update`）返回 `UNSUPPORTED` 并列出本机有的。关了的方向也照样提供格式，所以运行中可以再打开。C++ 的 `setAudioCodecs` 同时设置两个方向的列表。

## 发什么：`rcs_send_audio_config`

| 字段 | 内容 | 默认 |
| --- | --- | --- |
| `enabled` | 发不发 | 1（不挂扩展时：分享端发、观看端不发） |
| `codecs[]` | 提供的格式，按偏好 | Opus、AAC、PCM |
| `bitrate_bps` | Opus / AAC 的码率（PCM 忽略） | 128 000 |
| `source` | `RCS_AUDIO_SYSTEM`：本机正在播放的全部声音（只有分享端能用：观看端的机器在播的正是分享端的声音，传回去就是回声，`UNSUPPORTED`）；`RCS_AUDIO_HOST`：宿主用 `rcs_session_push_audio` 推（48 kHz 立体声交错 float32，1..48 000 帧，复制），或在 macOS、iOS 上把 `CMSampleBuffer` 原样交给 `rcs_session_push_sample_buffer`（ReplayKit 的应用声音和麦克风，SDK 各自转换、混成一路，[05](05-quickstart-ios.md#两种声音)）；`RCS_AUDIO_MICROPHONE`：一个麦克风 | `SYSTEM` |
| `exclude_current_process` | `SYSTEM`：不传本应用自己的声音（Core Audio 进程捕获和 WASAPI 进程环回还排除本进程启动的子进程，例如 Chromium、Electron 的辅助进程）；需要 `rcs_capabilities.audio_exclusion`（所有支持的 macOS；Windows 10 build 20348 / Windows 11 起） | 0 |
| `device` | `MICROPHONE`：`rcs_list_audio_devices` 列出的一个输入设备；0 是系统默认的输入，默认设备变了就跟着换。设备被拔掉时这个方向暂停、插回来自动恢复 | 0 |
| `voice_processing` | `MICROPHONE`：1 由系统消除回声、降噪（macOS 的 voice processing、Windows 的通信类别端点上驱动自带的、iOS 的语音处理、Android 的 `VOICE_COMMUNICATION` 预设）；系统做不到时照常录音，`RCS_EVENT_AUDIO` 的 `voice_processing` 说明是否真的在运行。接耳机、要原声（乐器、音乐）时设 0 | 1 |
| `sample_buffer_audio_percent`、`sample_buffer_microphone_percent` | `HOST` 推 sample buffer 时：设备和应用的声音（`RCS_SAMPLE_BUFFER_AUDIO`）、麦克风（`RCS_SAMPLE_BUFFER_MICROPHONE`）在混音里的音量，0..400 百分比；0 是不要这一种（它的缓冲照收、不发，另一种不等它）。运行中改立即生效，不重启声音 | 100、100（两种都发） |

**麦克风权限**：`rcs_microphone_permission` / `rcs_request_microphone_permission`（C++ `rcsdk::microphonePermission()` / `requestMicrophonePermission()`）。macOS 第一次请求时弹系统的提示（主线程），之后只能在系统设置里改；iOS 同样（任何线程），`Info.plist` 要有 `NSMicrophoneUsageDescription`；Windows 看“设置 › 隐私 › 麦克风”里是否允许桌面应用访问；Android 由宿主的 Activity 申请 `RECORD_AUDIO`，SDK 只报告状态（要先 `rcs_android_initialize`）。没有权限时这个方向暂停（`RCS_ERROR_PERMISSION_REQUIRED`），授权后自动开始。分享端的麦克风由助手录：助手在哪台机器、哪个用户会话里，就录哪里的。

## 收到怎么办：`rcs_receive_audio_config`

- `enabled`（默认 1：不挂扩展时观看端收、分享端不收）、`codecs[]`（接受的格式）、`playback`：1（默认）由 SDK 在系统默认输出设备上播放（插上耳机等切换设备时会跟着走）；0 时 SDK 不播放，宿主从 `on_audio` 拿解码后的声音（48 kHz 立体声交错 float32，`rcs_audio_frame`）自己播。`on_audio` 在两种模式下都会调用。
- **分享端收观看端的声音**：每个观看端的声音分别解码。SDK 播放时每个观看端一路，由系统混音；`playback = 0` 时 `on_audio` 分别交给宿主，`rcs_audio_frame.viewer_id` 说明是谁的，宿主可以自己混音、调音量、显示谁在说话。观看端之间不互相转发。
- 要编码的声音包（录制、转发）用 `RCS_EXT_ENCODED_OUTPUT` 的 `on_audio`（[17](17-replacing.md#编码包交给宿主录制转发审计)）。

## 回声和回环

- 观看端外放分享端的声音时，麦克风会把它录进去再传回去：`voice_processing` 由系统消掉（它消掉的是同一台机器播放的声音；iOS 上是经 SDK 的音频引擎播出的那部分）。
- **回环规则**：分享端发本机播放的声音（`SYSTEM`），又由 SDK 播放观看端的声音，观看端的声音就会被采集进去再传回给他们。所以这种组合要求排除本应用的声音（`exclude_current_process`，需要 `audio_exclusion`），而且助手就在会话的进程里（没有挂 `RCS_EXT_AGENT_LINK`）；否则 `start` / `update` 返回 `UNSUPPORTED`，原因里写明三个办法：排除本应用的声音、改发宿主的声音（`HOST`）、宿主自己播放（`playback = 0`）。SDK 不悄悄放任回声。

```cpp
// 远程协助：观看端说话，分享端听到
rcsdk::SessionConfig view(RCS_ROLE_VIEW);
if (rcsdk::microphonePermission() == RCS_PERMISSION_GRANTED) {
    view.setMicrophone();   // 系统默认的麦克风，开回声消除
}
rcsdk::SessionConfig share(RCS_ROLE_SHARE);
share.setReceiveAudio(true).setExcludeOwnSound(true);   // SDK 播放观看端的声音，发出去的不含它
// 或者：share.setReceiveAudio(true).setAudioPlayback(false); 并在 onAudio 里按 viewer_id 自己混音
callbacks.onAudio = [&](const rcs_audio_frame& f) { myMixer.write(f.viewer_id, f.samples, f.frames); };
```

## 采集

- **macOS**：默认用 Core Audio 进程捕获（macOS 14.2 起，每 5 ms 一块，周期可调）；14.0、14.1 上用 ScreenCaptureKit（每 20 ms 一块），权限与屏幕录制共用。
- **Windows**：WASAPI 环回，每个系统混音周期（通常 10 ms）一块；排除本应用的声音用进程环回（Windows 10 build 20348 / Windows 11 起）。
- **iOS**：不能录别的应用的声音：分享端发麦克风，或宿主推的 ReplayKit 给的应用声音和麦克风（`rcs_session_push_sample_buffer`，默认两种混成一路，[05](05-quickstart-ios.md#两种声音)）。
- **Android**：设备放的声音只能经 MediaProjection 的 AudioPlaybackCapture 在 Java 里录，由宿主推（[06](06-quickstart-android.md)）；`audio_capture_backends` 为 0。
- 采集后端和周期在 `rcs_audio_tuning_config`（`capture_backend`、`capture_period_frames`）；能力里的 `audio_capture_backends` 和 `audio_exclusion` 说明本机能怎样分享声音。
- 默认不排除本应用的声音（分享端自己的声音也传过去）。在同一进程里播放对方声音的宿主应打开 `exclude_current_process`，否则会有回声。
- **声音流不中断**：发送方没有声音时也持续发送静音（Opus 编码静音每包只有几个字节），观看端的缓冲一直就绪，声音响起时马上就能播出。

## 延迟、缓冲和丢包

- **延迟优先**：声音到了就播，不为对口型而等画面。播放缓冲只保留设备一次取数的量加上网络最近 8 秒的抖动和余量（有线的局域网上约 15～20 ms；Wi-Fi 省电时的打盹也算抖动，缓冲随之变长），开头 3 秒按至少 40 ms 的抖动准备，不超过 `max_delay_ms`；积压的声音直接丢掉，不会晚播。两台机器采样时钟的微小差异通过交叉淡化地丢弃或重复几毫秒来补偿。
- **冗余**（`rcs_audio_tuning_config.redundancy`，默认 1）：UDP、P2P、中继和宿主的数据报通道上，每个包再带一份上一个包（放得进一个数据报时；PCM 不带），单独丢掉的一个包由下一个原样补回（`rcs_audio_receive_stats.recovered_packets`），声音在路上的码率是两倍（`rcs_audio_send_stats.bitrate_bps`）。`LOW_BANDWIDTH` 预设不带。TCP 不丢包，从不带。
- 动态码率给视频定目标时先扣掉声音占用的码率。UDP 上声音单独成包、不重传、不经过视频的发送节流，在分享端的任何两个视频数据报之间都能插队；`rcs_network_tuning_config.audio_dscp` 可以给声音的数据报标 DSCP（默认不标，是取舍，见 [13](13-tuning.md#网络)）。
- **Wi-Fi 上不被大画面挤**：在 Wi-Fi 接入点的队列里，声音排在已经进去的视频后面。观看端量出路径比分享端发得慢时，大画面（关键帧、整屏滚动）超出路径约 8 ms 的部分按路径的速率发，留在分享端排队，声音插到前面（`rcs_video_tuning_config.path_pacing_percent`、`path_burst_us`，[13](13-tuning.md#动态码率)）。看声音被挤了多少：网络统计的 `sound_queue_us`（收到的声音在路径上排了多久：单向时延比最近 10 秒的最低值高多少，最近一到两秒的最大值）和 `sound_wait_us`（发出的声音在本端等了多久）；播放缓冲的 `target_ms`、`underruns` 是结果。
- **延迟的每一段都可以调**（`rcs_audio_tuning_config`，[13](13-tuning.md#声音)）：

  | 段落 | 字段 | 默认 | 调小的代价 |
  | --- | --- | --- | --- |
  | 采集周期 | `capture_period_frames`（只有 Core Audio 进程捕获可调，64..4800） | 240 帧（5 ms） | 唤醒更频繁，CPU 稍高 |
  | 播放缓冲余量 | `buffer_margin_us` | 2000 µs | 网络或 CPU 抖动时更容易断音 |
  | 播放缓冲上限 | `max_delay_ms` | 200 ms | 突发积压时更早丢弃；低于接收端 Wi-Fi 打盹的长度（100 ms 以上）时每次打盹都断音 |
  | 输出周期 | `playback_period_frames` | macOS 256 帧；Windows 系统混音周期 | 唤醒更频繁。Windows 只有在声卡支持更短的共享周期且混音为 48 kHz 浮点时才接受其他值 |
  | Windows 设备队列 | `playback_queue_frames` | 一个半周期 | 播放线程被叫醒晚了就会断音 |

  实际生效的值见统计：`rcs_audio_send_stats` 的 `capture_backend`、`capture_period_frames`，`rcs_audio_receive_stats` 的 `playback_period_frames`、`playback_queue_frames`、`buffer_ms`、`target_ms`。设备做不到的设置不会被悄悄换掉，而是让声音暂停并说明原因。
- **断音的两种原因**分开数：`underruns` 是缓冲空了（网络的问题），`device_underruns` 是缓冲里有声音、系统叫设备叫得太晚（机器忙），听的人都会听到一下咔哒。

## 运行中修改

两个声音扩展和 `RCS_EXT_AUDIO_TUNING` 运行中都可以整体替换：被改的方向停一下、按新设置重新开始，另一个方向、画面和控制不受影响。`enabled` 随时开关，对端收到这个方向的 `RCS_EVENT_AUDIO`；任一端都可以换格式列表（对端收到新的 `RCS_EVENT_PEER`；新列表里必须还有对端能用的，否则 `UNSUPPORTED`、什么都不变）。

## iOS 的声音会话

iOS 上 SDK 的声音跑在应用的一个 AVAudioEngine 上，AVAudioSession 由 SDK 管（默认）或交给宿主（`rcs_set_audio_session_config`），见 [05](05-quickstart-ios.md#声音会话)。其他系统上这两个函数是 `UNSUPPORTED`。
