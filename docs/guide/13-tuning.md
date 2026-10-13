# 13 预设、画面格式、动态码率与调优

大多数宿主选好预设就够了。本页讲预设、画面格式、动态码率，以及 `rcsdk/tuning.h` 的三个调优扩展。改调优参数之前先读 [PERFORMANCE](../PERFORMANCE.md)：头文件里每个字段的注释写了改它的代价，PERFORMANCE 里是实测的依据。

## 预设

预设决定**宿主没有放进扩展链的扩展**用哪套值。放进链的扩展完全是宿主的：预设不往里面填字段，不和它逐字段合并。

| 预设 | 场景 | 和 `BALANCED` 的主要不同 |
| --- | --- | --- |
| `RCS_PRESET_BALANCED`（默认） | 一般的远程桌面、远程协助和屏幕共享 | 就是各个 `*_INIT` 的值：H.264 硬件、1920×1080、60 FPS、20 Mbit/s 动态码率 |
| `RCS_PRESET_LOW_LATENCY` | 局域网里的交互：设计、游戏、操作 | 码率上限和起始码率更高，每帧发得更快，输入后加速采集，解码队列的时间上限和声音缓冲更短 |
| `RCS_PRESET_LOW_BANDWIDTH` | 手机热点、跨国的长路径 | 1280×720、30 FPS，码率上限、下限和起始码率更低，动态码率降得更早，声音码率更低、不带冗余、缓冲更长，连接存活的超时更长 |
| `RCS_PRESET_QUALITY` | 看文字、设计稿 | 4:4:4 排在 4:2:0 前面（两端都接受才用，否则按列表用下一个），码率更高，编码器每帧多花时间换画质 |

- 每个预设的具体取值见 [API-DESIGN 第 6 节](../API-DESIGN.md#6-预设的取值)。这些是建议值，还没有在实机上逐个量过。
- 预设给值的扩展只有 `RCS_EXT_SHARE_VIDEO`、`ENCODER`、`SEND_AUDIO`、`VIEW_REQUEST` 和三个调优扩展。共享什么、给不给控制、身份、服务器、观看端的解码器这些宿主的决定不随预设变：`QUALITY` 的观看端要自己在 `rcs_decoder_config.formats` 里列上 4:4:4（渲染器能画它才列）。
- 观看端的预设也起作用：`LOW_BANDWIDTH` 的观看端要求 1280×720、30 FPS、4 Mbit/s（`RCS_EXT_VIEW_REQUEST`），分享端照办时就降下来。
- **用某个预设、只改一个字段**：C 用 `rcs_preset_defaults(preset, &ext.header)` 拿到整份扩展再改；C++ 的设置函数和 `config.extension<T>()` 第一次碰到一个扩展时就从配置的预设开始，`rcsdk::defaults<T>(preset)` 给出一份。别的类型（包括模块和替换层的扩展）`rcs_preset_defaults` 返回 `INVALID_ARGUMENT`，它们的默认值就是 `*_INIT`。
- **看实际用了什么**：`rcs_session_get_settings(session, &ext.header)`（C++ `session.settings<T>()`）。
- **运行中换预设**：`rcs_session_set_preset`（C++ `session.setPreset`）立即作用于没有放进链的扩展。放进过链的扩展从此是宿主的。

```cpp
#include <rcsdk/tuning.hpp>

// 局域网里的远程控制：低延迟预设，其余默认
auto config = rcsdk::SessionConfig(RCS_ROLE_SHARE, RCS_PRESET_LOW_LATENCY)
                  .setAddress("0.0.0.0", 47990).setKey(key).setControl(true);
// 输入后的加速短一点：这个调优扩展从低延迟预设的值开始，只改一个字段，从此是宿主的
config.extension<rcs_video_tuning_config>().input_boost_ms = 50;
rcsdk::Session session(config);
session.start();
// 运行中网络变差：换成低带宽预设（上面放进链的视频调优不变），再看现在的画面设置
session.setPreset(RCS_PRESET_LOW_BANDWIDTH);
const auto video = session.settings<rcs_share_video_config>();
```

```c
/* 同样的事，C：低延迟预设的整份视频调优，改一个字段 */
rcs_video_tuning_config tuning = RCS_VIDEO_TUNING_CONFIG_INIT;   /* #include <rcsdk/tuning.h> */
rcs_preset_defaults(RCS_PRESET_LOW_LATENCY, &tuning.header);
tuning.input_boost_ms = 50;
config.preset = RCS_PRESET_LOW_LATENCY;
config.extensions = &tuning.header;
```

## 按场景配置

默认值面向一般情况：1920×1080、60 FPS、20 Mbit/s，H.264 硬件编解码，UDP（动态码率），声音开启（Opus），不允许控制，分享端自己的窗口和声音照常共享。下面是几类常见场景的起点，先选预设，再改宿主自己的决定：

| 场景 | 建议 |
| --- | --- |
| 先跑起来（局域网试用） | 只填核心配置。没有可用 GPU 的机器（部分虚拟机）上 `start` 返回 `UNSUPPORTED` 并说明缺了什么，由宿主决定是否改用 `RCS_ACCELERATION_SOFTWARE` |
| 画质优先、带宽有限 | 两端 `.setCodecs({RCS_CODEC_HEVC, RCS_CODEC_H264})`：两端都有 HEVC 时用 HEVC，同样画质的码率更低 |
| 只看不控的屏幕共享（协作、教学、演示） | 保持默认的不允许控制；不想让对方看到宿主自己的界面时 `.setExcludeOwnWindows(true)`；一次性的共享 `.setKeepListening(false)`；公网用 P2P，打不通时由宿主改用中继 |
| 远程桌面、远程协助（要控制） | 分享端 `.setControl(true)`，保持窗口可见（被隐藏的窗口仍能被点到）；局域网上可选 `LOW_LATENCY`。观看端 `.setCursorMode(RCS_CURSOR_SEPARATE)`，控制时把本机指针设成远端形状 |
| 同一进程里既分享、又播放对方的声音（双向共享、通话） | 分享端 `.setExcludeOwnSound(true)`（回环规则，[12](12-audio.md#回声和回环)） |
| 远程协助、教学：观看端说话 | 观看端 `.setMicrophone()`；分享端 `.setReceiveAudio(true)`，再 `.setExcludeOwnSound(true)`（SDK 播放）或 `.setAudioPlayback(false)` 并注册 `onAudio`（宿主播放） |
| 低延迟（设计、游戏、操作密集） | 两端 `LOW_LATENCY`；分享端 `.setVideo(w, h, 120, …)`；传输 UDP 或 P2P；观看端用 `onGpuFrame` 直接在 GPU 上显示、立即提交（[08](08-viewing.md#怎样画得快)） |
| 低带宽、移动网络 | 两端 `LOW_BANDWIDTH`；或分享端自己降低 `.setVideo(...)`、`.setRateMode(RCS_RATE_ADAPTIVE, 下限)`、`.setAudioBitrate(64'000)`。动态码率需要 UDP、P2P、中继或 HOST |
| 看文字、设计稿 | 两端 `QUALITY`，两端列出 HEVC 和 H.264；观看端的渲染器能画 4:4:4 时 `.setPictureFormats({RCS_PICTURE_444_8, RCS_PICTURE_420_8})` |
| 没有硬件编码器（虚拟机、云桌面、老机器），或要更低的码率 | 两端列出 AV1 或 VP9 和 H.264、`.setAcceleration(RCS_ACCELERATION_HARDWARE_FIRST)`：没有硬件时用 SDK 自带的 libaom / libvpx 在 CPU 上编（1080p60 约一个核），看文字的画面同样画质码率更低；一台机器跑很多会话时用 `software_threads` 压住每个会话的 CPU（[软件编解码](#软件编解码)） |
| 录制、审计、转发 | 观看端 `.use(rcsdk::encodedOutput(recorder))`，开始时 `requestKeyframe()`；只存不看用 `encodedOutput(recorder, false)`（[17](17-replacing.md#编码包交给宿主录制转发审计)） |
| 宿主自己显示和播放 | 观看端只注册 `onGpuFrame`（Windows 可用 `rcsdk::gpuDevice(device)` 共用设备）；`.setAudioPlayback(false)` 并注册 `onAudio` |
| 宿主自己提供画面、声音，自己处理输入 | `.setCaptureSource(RCS_CAPTURE_HOST)`、`.setAudioSource(RCS_AUDIO_HOST)`、`.setControl(true, RCS_INPUT_HOST)`（[07](07-sharing.md#宿主自己的画面和声音)） |
| 不需要声音 | 任意一端 `.setAudioEnabled(false)`（这一端两个方向都关） |
| 机器要一直连得上（无人值守、远程办公） | 保持唤醒模块，`while_running = 1`（[15](15-modules.md)、[16](16-unattended.md)） |
| 剪贴板、文件传输 | 官方模块 `rcsdk.clipboard`、`rcsdk.file-transfer`（[15](15-modules.md)）；默认关闭 |
| 标注、宿主自定义的命令 | 数据通道（[09](09-transports.md#数据通道)）或自己的模块 |

## 画面格式

默认的画面是 8 位、4:2:0、BT.709 矩阵、有限范围的 SDR，宿主什么都不用设。需要更好的画面时，两端各自列出接受的**画面格式**（`rcs_picture_format`），和编码格式一起协商：

| 格式 | 用途 | 编码格式 |
| --- | --- | --- |
| `RCS_PICTURE_420_8` | 默认 | 全部 |
| `RCS_PICTURE_420_10` | 渐变没有台阶 | HEVC Main 10、VP9 profile 2、AV1 Main |
| `RCS_PICTURE_444_8` | 每个像素都有完整的颜色：彩色文字、细线、表格 | H.264 High 4:4:4 Predictive、HEVC Main 4:4:4、VP9 profile 1、AV1 High |
| `RCS_PICTURE_444_10` | 4:4:4 加 10 位 | HEVC Main 4:4:4 10、VP9 profile 3、AV1 High |
| `RCS_PICTURE_HDR10_420` | HDR10：BT.2020 原色、PQ、BT.2020 矩阵、10 位 4:2:0 | HEVC Main 10、VP9 profile 2、AV1 Main |
| `RCS_PICTURE_HDR10_444` | HDR10 加 4:4:4 | HEVC Main 4:4:4 10、VP9 profile 3、AV1 High |

VP8 只有默认的 4:2:0 8 位。SDK 自带的 libvpx（VP9）、libaom 和 dav1d（AV1）在每个系统上都能做全部六种，所以要 4:4:4 或 10 位而硬件做不到时，可以用 VP9 或 AV1 加 `HARDWARE_FIRST`。

- **分享端**：`rcs_encoder_config.formats[]` 按偏好排列（C++ `setPictureFormats`），例如 `{RCS_PICTURE_444_8, RCS_PICTURE_420_8}` 表示“观看端接受就发 4:4:4，否则默认”。`matrix`（SDR 的 `BT709` 或 `BT601`）和 `range`（`LIMITED` 或 `FULL`）决定怎样写成 YCbCr（C++ `setColor`）；HDR10 总是 BT.2020 的矩阵。
- **观看端**：`rcs_decoder_config.formats[]` 是宿主的渲染器接受的格式，只有列出的才会收到（GPU 帧的布局会变：10 位是 P010，4:4:4 是 NV24 / P410；HDR10 的值是 PQ 编码的）。`rcs_view_request_config.format` 要求其中一种。CPU 帧（`on_frame`）总是 BGRA8 SDR，HDR10 和 `on_frame` 一起是 `UNSUPPORTED`。
- **协商**：每种编码格式的报价带着它在本机能做的画面格式（列出的 ∩ 实现的 `rcs_codec_implementation.picture_formats` ∩ 分享端采集能产生的 `rcs_capabilities.picture_formats`）；一种都做不了的编码格式不进报价。分享端选第一个两端都有、并且有共同画面格式的编码格式，再选自己 `formats[]` 里第一个两端都接受的画面格式（或观看端要求的那个）。有共同编码格式却没有共同画面格式时两端都以 `UNSUPPORTED` 结束，原因写明每种编码格式里两端各接受什么。
- **观看端收到什么**：`rcs_stream_info.format`、`color`（`rcs_color`：原色、传递函数、矩阵、范围）和 `hdr`（HDR10 的静态元数据 `rcs_hdr_metadata`，走流公告，不写进码流）；`RCS_EVENT_STREAM` 的 `RCS_STREAM_FORMAT` 位说明它们变了。每个 GPU 帧的 `format` 是布局、`color` 是值的含义：按 `color.matrix` 和 `color.range` 做 YCbCr→RGB，`color.transfer` 为 `RCS_TRANSFER_PQ` 时按 HDR 显示。
- **Windows 观看端的 4:4:4**：Windows 只在 CPU 上解 HEVC 4:4:4。观看端设 `RCS_ACCELERATION_HARDWARE_FIRST`，4:4:4 的流在 CPU 上解（交出 BGRA8 纹理），4:2:0 的流仍在 GPU 上解；1080p 复杂画面的软件解码每帧约 20 ms，跟不上 60 FPS 的满屏变化。
- 每个平台、每个编解码器能做什么、做不到什么（和原因），见 [PICTURE-FORMATS](../PICTURE-FORMATS.md)；本机的情况看 `rcsdk_cli --info` 列出的每个实现的格式。HDR10 现在只有接口，还没有实现报告它。

## 动态码率

分享端根据网络调整视频码率，默认开启（`rcs_share_video_config.rate_mode`：`DEFAULT` 是动态）。TCP 上也一样：TCP 自己的发送缓冲里的排队在观看端量到的延迟里看得见。上限是 `bitrate_bps`，下限是 `min_bitrate_bps`；规则、起始码率和发送节奏在 `RCS_EXT_VIDEO_TUNING`（`rcs_video_tuning_config`）里。

- **测量**：观看端约每 50 ms 报告一次收到了多少、丢了多少、视频的单向时延。分享端把单向时延和近 10 秒内的最小值比较，得出路径上的排队延迟。判断过载时取最近 `overuse_persist_ms`（150 ms）里各份报告的最小值：拥塞的队列一直在，接收端 Wi-Fi 省电时的打盹（100 ms 以上，每几秒一次）不会。丢包按最近 1 秒算。
- **规则**（括号里是默认值）：
  - 过载：持续的排队超过 `overuse_queue_ms`（30 ms）并在上升、持续超过 `severe_queue_ms`（60 ms），或丢包超过 `overuse_loss_permille`（100‰）：降到实际到达速率的 `decrease_percent`（85%），每次最多降一半、至少降 5%，两次下降至少隔 `decrease_interval_ms`（300 ms）。分享端发得少于码率的一半时（静止的屏幕），排队不算过载。
  - 通畅：排队低于 `clear_queue_ms`（10 ms）、丢包低于 `clear_loss_permille`（50‰）时上调。第一次过载之前每秒 `probe_increase_percent`（50%，从 `start_bitrate_bps` 起，0 是 4 Mbit/s；往返不到 100 ms 的路径按 100 ms / 往返时间加快，最多 8 倍：局域网上约半秒到 20 Mbit/s）；之后每秒 `recovery_increase_percent`（25%）回到这次过载开始时码率的 90%，再往上每秒 `increase_percent`（8%）；在那之上 10 秒没有过载就重新慢启动。
  - 默认值适合有线和 Wi-Fi；时延本身就大起大落的路径（蜂窝、卫星）适合调高排队阈值或拉长 `overuse_persist_ms`。
- **发送节奏**（固定码率也一样）：路径通畅时每一帧在编码出来后 `frame_spread_us`（1000 µs）内发完，最快不超过 `max_pacing_bps`（800 Mbit/s）；不通畅时改按码率的 1.5 倍发（固定码率 4 倍、至少 20 Mbit/s）。瓶颈或限速器会丢掉突发的路径可以把 `frame_spread_us` 调长或把 `max_pacing_bps` 调低。
- **按路径的速率发**（`path_pacing_percent`、`path_burst_us`）：观看端量出路径比分享端发得慢时（画面的分片被瓶颈摊开到达：Wi-Fi、慢的上行；量出的速率是网络统计的 `path_bps`），路径通畅时每一帧先发出相当于路径 `path_burst_us`（8000 µs）的量，其余最快按路径速率的 `path_pacing_percent`（110%）发，不低于上面的慢速。关键帧、整屏滚动这样的大画面因此在分享端排队，声音可以插到它前面，而不是在 Wi-Fi 接入点的队列里排在整帧后面；画面不会晚到，因为瓶颈本来就按它的速率放行。普通画面小于这个量，照旧一次发出。`path_burst_us` 越小，声音等得越少，但接入点攒的聚合帧越小、视频多占空口；`path_pacing_percent` 越接近 100（最少 110），大画面在接入点留下的队列越短，但路径变快时跟得越慢；0 关掉，回到只按上一条的发法。千兆有线上量不出路径速率（包之间太近），不受影响。模拟的测量见 [PERFORMANCE](../PERFORMANCE.md)。
- **丢包修复**（FEC，`fec_mode`、`fec_percent`；数据报传输上）：每帧的分片按 64 片一组，每组后面跟 Reed-Solomon 冗余片，丢的不超过冗余片数时当场补回。`AUTO`（默认）没丢包时不加冗余，丢包后按丢包率的 3 倍加（至少 10%，最多 `fec_percent`，默认 50），最后一次丢包 5 秒后停；`FIXED` 始终 `fec_percent`；`OFF` 不加。冗余算进码率。
- **参考帧失效**（`ref_invalidation`，默认 1）：冗余和补要都没救回的帧，能做到的编码器把它从参考里去掉，下一帧只参考观看端已有的帧、大小和普通帧一样（`RCS_EVENT_RECOVERY` 的 `method` 为 `REFERENCES`）；做不到的从关键帧继续。能做到的：编进来的库（OpenH264 除外）、驱动支持的 NVENC、oneVPL 的 H.264、HEVC、AV1（每帧给参考列表：保留最近 5 帧，丢帧后下一帧只参考丢失之前的那一帧；分层时不做）、VideoToolbox 的硬件编码器（低延迟模式；切到流水线编码器后不能）、接受应用控制长期参考帧的 Media Foundation 编码器（测试的 PC 上 NVIDIA、Intel 的硬件编码器和微软的软件 H.264 编码器都接受）。`rcs_codec_implementation.ref_invalidation` 列出能做到的，Media Foundation 的编码器要打开才知道（问一个要约 150 ms），列表里是 0；每一路流用的编码器能不能，看那一路的 `RCS_EVENT_CODEC` 的 `ref_invalidation`。
- **码率不够时让什么**（`degradation`）：编码器的输出超过目标码率（难以压缩的内容），或路径落后时，SDK 少采几帧，不让路径被丢包淹没。默认 `RCS_DEGRADE_FRAME_RATE` 只降帧率、尺寸不变，文字保持清楚，适合桌面、文档和代码；`RCS_DEGRADE_SIZE` 保帧率、画面按每边 75%、50%、38%、25% 逐档缩小（不低于 `min_size_percent`，默认 50）：2 秒里超过 30% 的时间被压住就降一档；离上次换档 10 秒以上、并且码率涨到降档时的 1.8 倍，或者 10 秒几乎没被压住、编码器只用了不到 40% 的码率时，升回一档（一直很重的内容在任何尺寸都会被压住，所以码率涨够了就升；升上去又被压住时按当时的码率再降，码率不再涨就不会来回跳），适合视频和游戏；`RCS_DEGRADE_BALANCED` 先让帧率降到一半，再缩尺寸。每次换档都是一路新的流（关键帧，观看端收到带 `RCS_STREAM_SIZE` 的 `RCS_EVENT_STREAM`，解码器按新尺寸继续）。视频发送统计的 `size_percent` 是当前每边的百分比，`limited_ms` 是这次运行里画面被压住的累计时间。
- **看得到的**：每次调整发 `RCS_EVENT_RATE`（`rcs_rate_event`：新目标、丢包、到达速率、排队延迟、往返时间）；视频发送统计的 `bitrate_bps` 是当前目标；网络统计里有往返、到达速率、丢包、排队延迟、观看端量出的路径速率（`path_bps`）、路径、冗余和补回的计数。
- **宿主自己决定**：`RCS_EXT_RATE_DECIDER` 拿到每份报告和规则的建议，可以改成自己的决定（[17](17-replacing.md#宿主决定码率)）。
- **换路径**（[09 网络变化](09-transports.md#网络变化连接迁移)）：换到地址不同的新路径时，单向延迟的基准、丢包和排队的历史清掉，码率从旧目标的 `path_change_percent`（默认 50%）再慢启动（0：从 `start_bitrate_bps`，同新连接）；只换了端口（NAT 重新映射）时保留原来的状态。换路径之前开始的报告不算，发往其他路径的探测不算丢包。
- 运行中随时可以改模式、上下限、起始码率和规则：切到 `FIXED` 立刻按 `bitrate_bps` 发送；改 `start_bitrate_bps` 把动态码率立刻移到那里。

## 调优扩展（`rcsdk/tuning.h`）

只对一个角色有意义的字段，另一个角色忽略。放进链的调优扩展完全是宿主的，所以从预设开始改。运行中都能整体替换，大多立即生效；各个超时和 `audio_dscp` 从下一个连接起生效，`local_interface` 让迁移的连接立即换到那块网卡。全部默认值见 [reference/defaults.md](reference/defaults.md)。

### 视频

`rcs_video_tuning_config`（`RCS_EXT_VIDEO_TUNING`）：

- 动态码率的规则（上文）、`start_bitrate_bps`、换路径后从哪里起步 `path_change_percent`（上文）、码率不够时让什么 `degradation` / `min_size_percent`（上文）、发送节奏 `frame_spread_us` / `max_pacing_bps` / `path_pacing_percent` / `path_burst_us`、丢包修复 `fec_mode` / `fec_percent` / `ref_invalidation`。
- `input_boost_ms`（0..1000，默认 0）：每次远程输入被执行后，在这段时间内以两倍帧率采集编码，让输入的效果更快到达（局域网实测约快 4 ms；`LOW_LATENCY` 是 100）。
- `speed`：编码器每帧花多少时间换画质。`RCS_ENCODER_FASTEST`（默认，延迟最低：NVENC P1、oneVPL 目标用途 7（`BALANCED` 4、`BEST_QUALITY` 1）、Media Foundation 0、VideoToolbox 速度优先、MediaCodec 最高运行速率、libvpx VP9 速度 9 / VP8 -10、libaom 10、OpenH264 低复杂度）、`BALANCED`、`BEST_QUALITY`（同样码率画质最好、每帧更慢）。运行中改会重启编码器。
- `content`、`software_threads`：只对 SDK 自带的编解码库有用，见下面的[软件编解码](#软件编解码)。
- `keyframe_interval_ms`（0：只在需要时；100..60 000：至少这么频繁）、`keyframe_min_interval_ms`（默认 500：同一档编码有几个观看端时，观看端要的和新加入的关键帧之间至少隔多久，期间的合并成一个）。
- 几个观看端时（[指南 07](07-sharing.md#弱的观看端多一档编码rcs_ext_encodings)）：`viewer_keyframe_interval_ms`（默认 2000：会话里有别的观看端时，一个观看端的丢包或积压引起的关键帧最多这么频繁，多出来的请求等下一个；它一个人在那一档时也这样：它的关键帧占着大家共用的上行，路径又丢了的关键帧多要几个也补不回来。只有一个观看端的会话不受限）、`loss_keyframes_move`（默认 3：同一档有别人时，一分钟里引起这么多个就去低档；0 不移）、`move_down_ms`（默认 2000：路径装不下这一档实际发出的这么久就下移）、`move_up_ms`（默认 10 000：装得下上一档这么久就上移）、`lower_idle_ms`（默认 10 000：`AUTO` 的低档没人用这么久就停）。
- 观看端的解码队列 `queue_frames`（默认 64）、`max_queue_age_ms`（默认 80 ms，0 不按时间丢）：超出时丢弃积压、请求关键帧。时间上限决定延迟，帧数上限只限制内存；不建议用很小的帧数，否则高帧率下一阵突发就会触发恢复。录制服务器可以放宽它们（用延迟换完整）。

### 声音

`rcs_audio_tuning_config`（`RCS_EXT_AUDIO_TUNING`）：采集后端 `capture_backend`（`AUTO`）和周期 `capture_period_frames`（只有 Core Audio 进程捕获可调）；冗余 `redundancy`（1）；播放缓冲上限 `max_delay_ms`（200 ms）和余量 `buffer_margin_us`（2000 µs）；输出周期 `playback_period_frames`、Windows 的设备队列 `playback_queue_frames`。每一项的代价见 [12](12-audio.md#延迟缓冲和丢包)。

### 网络

`rcs_network_tuning_config`（`RCS_EXT_NETWORK_TUNING`）：

| 字段 | 管什么 | 默认（范围） |
| --- | --- | --- |
| `local_interface` | 只用这块网卡（Wi-Fi 和有线并存时只走 Wi-Fi、不走 VPN）：P2P 和中继两端都管，UDP 和 TCP 直连管观看端（分享端听的是 `connect.address`，填这块网卡的地址即可）。macOS 是 BSD 名字（`en0`），Windows 是适配器的名字（`WLAN`）或编号，Android 是名字（`wlan0`；交出 Java VM 时按网络标记套接字（`android_setsocknetwork`），否则 `SO_BINDTODEVICE`，Linux 5.7 起的内核，Android 是否照它路由没有实测）。这块网卡没了（或没有可用地址）时，迁移的连接路径算没了（`RCS_PATH_LOST`），等它回来。按网络种类挑用 `rcs_paths_config`（[09](09-transports.md#网络变化连接迁移)），两者不能同时设 | 空：所有网卡 |
| `wifi_low_latency` | 连上期间让本机的 Wi-Fi 不省电（省电的 Wi-Fi 每几秒让无线电睡 100 ms 以上，画面和声音一阵一阵地到，动态码率还会把它当成拥塞）。Android：低延迟 Wi-Fi 锁（要 `WAKE_LOCK` 和交出的 Java VM，系统只在应用在前台、亮屏时生效）；Windows：WLAN 媒体流模式、不做后台扫描。Apple 的系统没有这个设置（`rcs_capabilities.wifi_low_latency` 为 0）。是否保持着：`rcs_network_stats.wifi_low_latency` | 1 |
| `audio_dscp` | UDP、P2P 和走 UDP 的中继：声音的数据报带这个 DSCP（1..63；46 是 EF），接入点按它放进 Wi-Fi 的优先队列。是取舍：家用 2.4 GHz 网状 Wi-Fi 上缓冲放空的次数少一半，但补不回的声音丢包多 2～5%（[PERFORMANCE](../PERFORMANCE.md)）；有 QoS 管理的企业网络适合打开。实际标上的：`rcs_network_stats.marked_packets` | 0：不标 |
| `send_queue_bytes` | 数据通道的发送队列，装不下时 `rcs_session_send_data` 返回 `QUEUE_FULL` | 64 MiB（1 MiB～1 GiB） |
| `in_flight_bytes` | UDP、P2P、中继和 HOST 上同时在路上、还没确认的数据通道字节 | 64 KiB（4 KiB～16 MiB） |
| `reconnect_window_ms` | 观看端路径断了以后，留着连接等它回来、再重连，一共多久（从断开算） | 30 秒（1 秒～1 小时；0：一直试到 `stop`） |
| `reconnect_interval_ms` | 观看端两次重连之间隔多久 | 1 秒（0.1～60 秒） |
| `connect_timeout_ms` | 一次握手最多等多久；观看端到分享端的每条 TCP 连接、到 TURN 服务器的 TCP 或 TLS 连接最多等多久 | 5 秒（1～60 秒） |
| `idle_timeout_ms` | 多久没收到对端任何数据就算路径没了（`RCS_PATH_LOST`：连接按 `rcs_paths_config` 留着，或以 `RCS_ERROR_NETWORK` 结束） | 5 秒（1～60 秒） |
| `path_stall_ms` | 当前路径多久没收到任何东西（每 250 ms 一个 PING）就开始找别的路径（`RCS_PATH_CHANGING`）：停发视频、声音和数据，放开观看端按住的键，有备用路径时换过去。比省电的 Wi-Fi 打盹（100 ms 以上）长，免得误换 | 1 秒（0.2～10 秒） |
| `path_hold_ms` | 分享端：一个观看端的路径没了以后，留着它（编号、准入、模块、在路上的消息）等它从别的路径回来多久；名额不够时先让给新来的观看端。观看端留多久看 `reconnect_window_ms` | 30 秒（1 秒～1 小时） |
| `standby_probe_ms` | 备用路径（`rcs_paths_config.standby`）多久探测一次，保持它的 NAT 映射、量往返；运营商 NAT 的 UDP 映射常在 30 秒左右回收 | 15 秒（1～60 秒） |
| `ice_gather_ms` | P2P 和中继：给出信令之前收集候选地址最多多久 | 1.5 秒（0.2～10 秒） |
| `ice_connect_ms` | P2P 和中继：连通检查找路径最多多久，之后以 `RCS_ERROR_NETWORK` 失败 | 15 秒（1～120 秒） |
| `ice_addresses` | P2P：除了网卡的地址，还把哪些地址作为候选给出（`rcs_ice_addresses` 的位）。`RCS_ICE_ADDRESSES_LOOPBACK`：本机的 127.0.0.1 和 ::1，同一台机器上的两端不用网络也能连上（测试）。对端信令里的地址都会被探测，所以每一端剔除（写日志，失败原因里列出）没有对端会在的地址：未指定、组播、广播和保留地址、云元数据服务（169.254.169.254、100.100.100.200、168.63.129.16、fd00:ec2::254）；环回和链路本地地址只在本端自己也有同一范围的候选（这个选项，或者 STUN、TURN 服务器就在那里）时才用；同一个 IP 最多 4 个候选 | 0 |
| `turn_allocate_ms` | 中继：TURN 服务器批下分配最多等多久 | 5 秒（1～30 秒） |

```cpp
#include <rcsdk/tuning.hpp>

// 蜂窝网络上的分享端：低带宽预设；时延本身起伏大，排队阈值调高；多等一会儿才算断线
auto config = rcsdk::SessionConfig(RCS_ROLE_SHARE, RCS_PRESET_LOW_BANDWIDTH).setAddress("0.0.0.0", 47990).setKey(key);
auto& video = config.extension<rcs_video_tuning_config>();   // 从 LOW_BANDWIDTH 的值开始
video.overuse_queue_ms = 60;
video.severe_queue_ms = 120;
config.extension<rcs_network_tuning_config>().idle_timeout_ms = 15'000;
```

```c
#include <rcsdk/tuning.h>

/* P2P 的观看端：5 秒找不到直连路径就失败，宿主再改走中继 */
rcs_network_tuning_config network = RCS_NETWORK_TUNING_CONFIG_INIT;
rcs_preset_defaults(config.preset, &network.header);   /* 从配置的预设开始 */
network.ice_connect_ms = 5000;
network.header.next = config.extensions;
config.extensions = &network.header;
```

## 软件编解码

SDK 自带的编解码库（`RCS_BACKEND_LIBVPX`：VP8、VP9 编解码；`LIBAOM`：AV1 编码；`DAV1D`：AV1 解码；`OPENH264`：H.264 编解码，默认不编进去）在每个系统上行为相同：

- **编码**：恒定码率、不前瞻、自己不丢帧、只在要求时出关键帧（和硬件编码器一样）；libvpx 和 libaom 能做参考帧失效（丢包后不出关键帧，只参考丢包之前的帧），OpenH264 不能。
- **`content`**（`rcs_video_tuning_config`）：`DETAIL`（文字、界面：libvpx 的屏幕内容模式，libaom 的调色板和关键帧的帧内块复制）或 `MOTION`（视频、游戏、摄像头）；`AUTO`（默认）对显示器、区域、窗口、应用是 `DETAIL`，对宿主的画面（`RCS_CAPTURE_HOST`）是 `MOTION`。硬件编码器没有这类工具，忽略它。
- **`software_threads`**：每个库编码器、解码器用几个线程，0（默认）是逻辑核的一半、最多 8 个。云桌面一台机器跑很多会话时调小，压住每个会话的 CPU。
- **大约的开销**（M4 Pro，1080p，测试画面）：VP9 编码每帧约 10 ms、AV1 约 10～19 ms，解码 AV1 约 3.6 ms；1080p60 软件编码约占 0.8～1.2 个核。4K60 软件编码跟不上，用硬件。
- 构建时可以去掉某个库（`RCSDK_WITH_LIBVPX`、`RCSDK_WITH_DAV1D`、`RCSDK_WITH_LIBAOM`、`RCSDK_WITH_OPENH264`，[BUILDING](../BUILDING.md)）：它的实现就不在列表里，选它的后端以原因 `UNSUPPORTED`。

## 实现的选择

- `rcs_list_codec_implementations` 列出本机的每一个编解码实现（`rcs_codec_implementation`）：编码格式、方向、硬件或软件、后端、所在 GPU 的 LUID（Windows）、能接受的最大画面、能做的画面格式、能不能做参考帧失效、名称（例如 `NVIDIA HEVC Encoder MFT`、`Apple HEVC (HW)`）。`rcs_get_capabilities` 是它的摘要。第一次列出要逐个打开编解码器、解码探针（Mac 约 0.2 秒、RTX 4090 的 PC 约 0.4 秒、S25+ 约 0.6 秒），之后进程里一直缓存：要第一次连接也快的宿主，可以在启动时在后台线程里先调一次（Android 上 `rcs_android_initialize` 已经这样做了）。
- 宿主用 `rcs_encoder_config` / `rcs_decoder_config` 的“编码格式 + 加速方式 + 后端”选择实现。后端：`RCS_BACKEND_MEDIA_FOUNDATION`（Windows，任何厂商的 GPU，以及微软的 H.264 软件编码器）、`RCS_BACKEND_NVENC`（Windows，只编码、只在 NVIDIA GPU 上，驱动 522.25 起）、`RCS_BACKEND_NVDEC`（Windows，只解码、只在 NVIDIA GPU 上，只解 Media Foundation 在 GPU 上解不了的 4:4:4：HEVC，GPU 报告它解时；画面是 BGRA8 纹理）、`RCS_BACKEND_ONEVPL`（Windows，只在 Intel GPU 上：显卡驱动带的 oneVPL 运行时，Tiger Lake 起；编码 H.264、HEVC 含 4:4:4、AV1，解码 H.264、HEVC、VP9、AV1，含 GPU 报告它解的 4:4:4，4:4:4 的画面是 BGRA8 纹理）、`RCS_BACKEND_VIDEOTOOLBOX`（macOS、iOS）、`RCS_BACKEND_MEDIACODEC`（Android）、SDK 自带的库（上一节）、`RCS_BACKEND_AUTO`（默认：每种格式取该 GPU 排在第一的实现；NVIDIA GPU 上 NVENC 排在前面，在 RTX 4090 上每帧编码快 1～2.5 ms；Intel GPU 上 oneVPL 排在前面，在 Core Ultra 7 270K Plus 的核显上每帧编码 p50 快约 0.3～1 ms、p95 快 2～15 ms（见 [PERFORMANCE](../PERFORMANCE.md) 的“Windows：NVDEC 解 4:4:4、直接调 oneVPL”）；解码器 Media Foundation 排在 NVDEC 前面，NVDEC 排在 oneVPL 前面，`HARDWARE_FIRST` 时 4:4:4 的流交给解码设备那块 GPU 上的 NVDEC 或 oneVPL，4:2:0 留给 Media Foundation（Intel 核显上它每帧比 oneVPL 快 0.5～3 ms），Windows 没装某编码格式的解码器时交给 oneVPL；软件里自带的库排在系统的前面，H.264 除外）。`RCS_BACKEND_HOST` 只用于报告宿主自己的编解码器。
- Windows 的 Media Foundation 硬件编码器：NVIDIA 的只在 NVIDIA 显卡是 Windows 给程序的默认 GPU 时起得来，否则不列出（混合显卡见 [04](04-quickstart-windows.md)）；Intel 的在恒定码率下不跟随码率变化，SDK 给它们用峰值等于目标的 VBR（见 [PERFORMANCE](../PERFORMANCE.md) 的“Windows：混合显卡上的 Media Foundation 编码器”）。
- 指定的后端本机没有时，创建会话（或 `rcs_session_update`）返回 `UNSUPPORTED`；分享的画面不在它能用的 GPU 上时（例如选了 NVENC，显示器接在 Intel 核显上），开始编码失败并说明原因，不会换成别的后端。
- 实际使用的实现可以从 `RCS_EVENT_CODEC` 和统计（`rcs_video_send_stats.encoder`、`rcs_video_receive_stats.decoder`）得知。
