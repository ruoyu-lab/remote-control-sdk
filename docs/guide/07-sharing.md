# 07 分享端

分享端决定发什么画面（尺寸、帧率、码率、编码格式）、共享什么（显示器、区域、窗口、应用，或宿主自己的画面和码流）、服务几个观看端、谁能进来。远程控制见 [11](11-input.md)，声音见 [12](12-audio.md)，码率和发送的细节见 [13](13-tuning.md)。

## 发什么画面：`rcs_share_video_config`

| 字段 | 内容 | 默认（`BALANCED`） |
| --- | --- | --- |
| `width`、`height` | 编码尺寸：偶数，至少 16×16，每边最多 8192、最多 35 651 584 像素（8192×4352）。共享的东西按原比例缩放进去，多余的部分留黑边 | 1920×1080 |
| `frames_per_second` | 1..240 | 60 |
| `bitrate_bps` | 128 kbit/s..500 Mbit/s；固定码率时就是码率，动态码率时是上限 | 20 Mbit/s |
| `rate_mode` | `RCS_RATE_DEFAULT`（动态，同 `ADAPTIVE`）、`FIXED`、`ADAPTIVE` | `DEFAULT` |
| `min_bitrate_bps` | 动态码率的下限；0 是 500 kbit/s（不超过 `bitrate_bps`） | 0 |
| `follow_requests` | 1：照办观看端的要求（`rcs_view_request_config`），在下面的上限之内；0：只用本端的设置 | 1 |
| `max_width`、`max_height`、`max_frames_per_second`、`max_bitrate_bps` | 观看端要求的上限；0：显示器的像素尺寸、显示器的刷新率（系统不说时 60）、200 Mbit/s | 0 |

- 上限只约束观看端的要求，不约束分享端自己的设置。超过上限、编码格式或两端编解码器能接受的要求，按原比例缩小到放得下（例如 5K 显示器、H.264 时要 5120×2880 得到 4096×2304）。
- **尺寸的上限按编码格式**，横竖都一样：H.264 每边最多 4096、最多 36 864 个宏块（4096×2304 或 2304×4096）；HEVC、VP8、VP9、AV1 和宿主自己的格式每边最多 8192、最多 35 651 584 像素。具体的编码器、解码器可能更小：`rcs_codec_implementation.max_width` / `max_height` 是本机每个实现能接受的，对端的在 `rcs_peer_info.codec_limits`。
- **帧率的上限按编码器**：分享端的编码器在一种画面格式下每秒能编的像素数有上限（它支持的最高 Level，`rcs_codec_implementation.max_pixels_per_second`，系统说了才有，现在只有 NVENC）。流的尺寸乘帧率超过它时，帧率降到它能编的，尺寸不变；`rcs_stream_info.frames_per_second` 是实际的。例如 RTX 4090 上 H.264 4:4:4 的 4K 最多 128 帧，要 160 帧时给 128；4:2:0 和 HEVC 照样 160。
- **运行中改了编码器却打不开**（换编码格式、尺寸、帧率、画面格式时，编码器还是拒绝了）：这次改动不生效，流按原来的继续跑，宿主收到 `RCS_EVENT_ENCODING`（`encoding` 0，`running` 1，字段是想改成的样子，`result`、`message` 说为什么），同样的设置在变之前不再试。改的是共享什么（`rcs_capture_config`）而失败的，运行照旧结束：不会退回去共享宿主已经不想共享的东西。
- **分享端自己设的尺寸从不被缩小**：本端提供的格式（连着观看端时还有观看端的）没有一个能接受时，`start` / `rcs_session_update` 返回 `UNSUPPORTED`；协商出的格式接受不了时这个连接在 `RUNNING` 之前结束，两端都是 `UNSUPPORTED`，原因写明格式和它的上限。所以要发大画面的分享端把能接受它的格式（HEVC、VP9、AV1）排在前面。
- 实际发送的画质经 `RCS_EVENT_STREAM` 和 `rcs_session_get_stream_info` 告诉两端（`rcs_stream_info`），事件的 `changes` 位说明这次变了什么。画质变化时编码器带关键帧重新开始，连接不断。
- 不想让观看端决定时设 `follow_requests = 0`，或用 `max_*` 收紧上限（比如最多 1080p、30 FPS）。

## 编码：`rcs_encoder_config`

- `codecs[]`：提供的编码格式，按偏好排序，最多 8 个（`RCS_CODEC_H264`、`HEVC`、`VP8`、`VP9`、`AV1`，宿主自己的格式 16..31，见 [17](17-replacing.md)）；默认只有 H.264（几乎所有 GPU 都能硬件编码）。HEVC、VP9 和 AV1 同样画质需要的码率更低，两端都有时才用得上。VP8 只有 4:2:0 8 位、跟码率不如 VP9，给只有它的对端。
- **软件编码不靠系统**：SDK 自带编解码库（libvpx：VP8、VP9；libaom：AV1 编码；dav1d：AV1 解码；Linux 上还有 OpenH264：H.264），每个系统上都能用 CPU 编解这些格式。1080p60 软件编码大约占一个核；`rcs_video_tuning_config` 的 `content`（文字界面 / 视频）和 `software_threads`（线程数）只对它们有用（[13](13-tuning.md#软件编解码)）。构建时可以去掉某个库（`RCSDK_WITH_LIBVPX` 等，[BUILDING](../BUILDING.md)）。
- `formats[]`：画面格式（4:2:0 / 4:4:4、8 / 10 位、SDR / HDR10），按偏好排；`matrix`（`RCS_MATRIX_BT709` 或 `BT601`）、`range`（`LIMITED` 或 `FULL`）决定怎样写成 YCbCr。默认 8 位 4:2:0、BT.709、有限范围。见 [13 画面格式](13-tuning.md#画面格式)和 [PICTURE-FORMATS](../PICTURE-FORMATS.md)。
- `acceleration`：`RCS_ACCELERATION_HARDWARE`（默认）、`SOFTWARE`，或 `HARDWARE_FIRST`：每路流开始时，被共享的显示器所在的 GPU 能编它的格式、画面格式和尺寸就用硬件，否则用软件（例如 GPU 只编 VP9 4:2:0 时，VP9 4:4:4 用 libvpx；没有硬件编码器的虚拟机上全用软件）；提供的是两者合起来的格式。没有硬件编码器的机器上默认配置 `start` 以 `UNSUPPORTED` 失败，由宿主决定是否改用软件或硬件优先。
- `backend`：`RCS_BACKEND_AUTO`（默认：每种格式取实现列表里该 GPU 排第一的；NVIDIA GPU 上是 NVENC，有 oneVPL 运行时的 Intel GPU 上是 oneVPL（H.264、HEVC、AV1），其他是 Media Foundation；软件里 SDK 自带的库排在系统的前面，H.264 除外），或指定一个（`rcs_list_codec_implementations` 列出本机的；`RCS_BACKEND_LIBVPX`、`LIBAOM`、`DAV1D`、`OPENH264` 是自带的库）。被共享的显示器不在这个后端能用的 GPU 上时，开始编码失败并说明，不会换成别的后端。
- 编码器的速度和定期关键帧是调优（`rcs_video_tuning_config`，[13](13-tuning.md)）。

## 共享什么：`rcs_capture_config`

`source` 决定 SDK 采集什么，默认是主显示器。本机支持哪些看 `rcs_capabilities.capture_sources`（`1u << source` 位）：macOS 四种都有；Windows 上单个窗口要 Windows 10 1903 起；iOS、Android 只有 `HOST` 和 `ENCODED`。

| `source` | 填写 | 画面 | 远程输入 |
| --- | --- | --- | --- |
| `RCS_CAPTURE_DISPLAY` | `display_id`（0：主显示器） | 整个显示器 | 映射到整个显示器 |
| `RCS_CAPTURE_REGION` | `display_id`，`region_x/y/width/height`（显示器像素，至少 16×16，必须在显示器内） | 显示器的这一块 | 映射到这一块 |
| `RCS_CAPTURE_WINDOW` | `window_id`（`rcs_list_windows` 给出） | 这一个窗口，被挡住、跑到屏幕外的部分也有；同一应用的菜单、对话框不在画面里 | 映射到窗口现在的位置，只进这个窗口 |
| `RCS_CAPTURE_APPLICATION` | `process_id`，`display_id` | 这个进程在该显示器上的全部窗口（含菜单、对话框），桌面和其他应用是黑的；被别的应用挡住的部分 macOS 照样画出，Windows 是黑的 | 映射到整个显示器，只进这个应用的窗口 |
| `RCS_CAPTURE_HOST` | — | 宿主推的画面（见下文） | 映射到 `display_id` 那块显示器（或交给宿主） |
| `RCS_CAPTURE_ENCODED` | — | 宿主推的已编码的码流（[17](17-replacing.md)） | 同上 |

- **显示器**：`rcs_list_displays`（本进程所在的机器）和 `rcs_session_list_displays`（会话采集的机器：助手在另一个进程时是助手那台）列出 `rcs_display_info`：编号（显示器保持连接期间不变）、像素尺寸、刷新率、旋转、是不是主显示器、在桌面上的位置。显示器增减或变了模式有 `RCS_EVENT_DISPLAYS`。旋转的显示器照常共享，画面是正的。
- **窗口**：`rcs_list_windows` / `rcs_session_list_windows`（助手在另一个进程时去问它，一秒内没答复是 `TIMEOUT`）列出 `rcs_window_info`：从前到后，`window_id`、`process_id`、所在显示器和位置大小、是否最小化、是否前台、标题和应用名。窗口随时变化，两次调用之间可能多出来（`BUFFER_TOO_SMALL`，C++ 的 `rcsdk::windows()` 会自动再要一次）。macOS 没有屏幕录制权限时标题是空的。SDK 不画选择界面。
- **缩略图**（给“选择要共享的东西”的界面）：`rcs_capture_picture(what, max_width, max_height, out)`（`rcsdk/module.h`）按一份 `rcs_capture_config` 在本机取一张现在的画面：BGRA8，按比例缩到 `max_width × max_height` 以内（各 1..8192），不放大，不含指针，写进调用方的缓冲区（`rcs_picture`；最多 `4 × max_width × max_height` 字节，不够时 `BUFFER_TOO_SMALL` 并给出需要的大小）。每次就地采一张，几十毫秒，不要在界面线程上调用。采集会暂停的情况（窗口最小化或关了、应用没有窗口、锁屏）是 `INVALID_STATE` 加原因。在会话采集的机器上取（助手在另一个进程时）用缩略图模块 `rcs_thumbnail_capture`（[15](15-modules.md)）。C++：`rcsdk::capturePicture(capture, w, h)`。
- **暂停而不结束**：共享的东西暂时看不到时采集暂停，连接和观看端保持，观看端停在最后一帧，`RCS_EVENT_CAPTURE` 的 `paused` 说明原因（`rcs_capture_pause`）：`HIDDEN`（最小化、隐藏、应用在这个显示器上一个窗口也没有、区域跑出显示器，重新出现后自动恢复）、`GONE`（窗口关了、应用退出了，不会自己恢复：宿主用 `rcs_session_update` 换一个，或者停止）、`DESKTOP`（锁屏、UAC 安全桌面、显示器睡眠或换模式、显示器暂时断开；断开 10 秒以 `RCS_ERROR_CAPTURE` 失败）、`AGENT`（助手不在）。
- **跟随窗口**：窗口改变大小时画面跟着变，在帧里保持比例（`RCS_EVENT_STREAM` 带 `RCS_STREAM_CONTENT` 和 `RCS_STREAM_DISPLAY`）。
- **输入只进共享的东西**（`WINDOW`、`APPLICATION`）：按下鼠标键或滚轮时，那一点前面是别的窗口就先把共享的窗口提到最前，再执行；那一点没有共享的东西或提不上来，这个事件不执行（`RCS_ERROR_INVALID_STATE`，记入输入统计）。按键和文字只在共享的应用有键盘焦点、并且（`WINDOW`）共享的窗口是它最前面的窗口（Windows 上也可以是这个窗口拥有的对话框）时执行。不管焦点在哪都会作用到共享的东西以外的键，限定期间一律不执行（`RCS_ERROR_INVALID_STATE`）：媒体键，以及系统自己的快捷键——Windows 上的 Windows 键、Alt+Tab、Alt+Esc、Ctrl+Esc、Ctrl+Shift+Esc 和 Print Screen（整个屏幕进剪贴板），macOS 上的 Command-Tab、Command-空格、Command-Option-Esc、Control-Command-Q、Shift-Command-Q、Shift-Command-3 到 6（截屏）和 Control 加方向键。松开总会执行，不会卡键。别的应用或用户自己注册的全局快捷键（macOS 上还有切换输入法的 Control-空格之类）在共享的应用里按下时照样会触发它的主人，和在那台机器前按一样：限定只保证输入落在观看端看到的地方，不是安全边界，不能用来限制一个允许打字的观看端（它能做那个应用能做的一切）。
- **`exclude_current_process`**（默认 0）：在画面中隐藏本应用的窗口，包括共享期间新开的通知、对话框和菜单；只对 `DISPLAY`、`REGION` 有意义。Windows 上可见的窗口隐藏不了时本次运行以 `RCS_ERROR_CAPTURE` 失败，不会照常共享出去。被隐藏的窗口观看端看不到，却仍然能被点到：要给对方控制时不要隐藏。
- **`secure_desktop`**（Windows，默认 0）：采集和远程输入跟随登录屏、锁屏、UAC 等安全桌面，要 LocalSystem 身份，见 [16](16-unattended.md)。只能和 `DISPLAY`、`REGION` 一起用。
- 运行中换共享的东西：`rcs_session_update` 一个新的 `rcs_capture_config`，采集在新设置下重新开始，远程输入也跟着换过去。

```cpp
// 列出窗口，每个配一张缩略图，让用户选一个，然后只共享它
const auto windows = rcsdk::windows();
for (const auto& w : windows) {
    auto what = rcsdk::defaults<rcs_capture_config>();
    what.source = RCS_CAPTURE_WINDOW;
    what.window_id = w.window_id;
    try {
        showChoice(w, rcsdk::capturePicture(what, 320, 200));   // BGRA8，pixels.size() == width * height * 4
    } catch (const rcsdk::Error& e) {
        showChoice(w, e.result() == RCS_ERROR_INVALID_STATE ? "最小化了" : "没有预览");
    }
}
auto config = rcsdk::SessionConfig(RCS_ROLE_SHARE).setAddress("0.0.0.0", 47990).setKey(key)
                  .setCaptureWindow(windows[picked].window_id).setControl(true);
rcsdk::Callbacks callbacks;
callbacks.onEvent = [&](const rcs_event& e) {
    if (e.type == RCS_EVENT_CAPTURE && e.capture.paused == RCS_CAPTURE_PAUSE_GONE) {
        showMessage("共享的窗口关了，请重新选择");   // 选好后：session.update(新的 rcs_capture_config)
    }
};
// 也可以：setCaptureRegion(displayId, x, y, w, h)、setCaptureApplication(processId, displayId)、setCaptureDisplay(displayId)
```

## 多显示器

被控的机器有几块显示器时，分享端可以同时发最多 `RCS_MAX_STREAMS`（4）路画面，每块显示器一路（`RCS_EXT_STREAMS`，`rcs_streams_config`）。第 0 路是 `RCS_EXT_CAPTURE` 的（这时它必须共享显示器），`displays[]` 按顺序给出第 1、2、3 路的显示器（用过的在前，不用的填 0）。不挂这个扩展时只有第 0 路。

- **每路各自采集、编码**：尺寸和帧率按同一份设置（`rcs_share_video_config`、主观看端的要求）适配各自的显示器；编码格式、画面格式是第 0 路的。
- **码率**：总码率（固定的，或动态码率的目标）按 `weights[]` 分给各路（第 0 路在前；全 0 按各路的像素）；一秒没有新画面的路按四分之一的权重算，把份额让给在动的路。
- **每路有自己的**：公告（`RCS_EVENT_STREAM` 的 `stream_index`，`rcs_session_get_stream_info(session, stream, &info)`）、帧（`rcs_frame.stream`、`rcs_gpu_frame_info.stream`、`rcs_encoded_packet.stream`）、编解码器、采集事件（`rcs_event.stream_index`）、统计（`rcs_session_get_stream_stats`）。`rcs_stream_info.desktop_x/y/width/height` 是这块显示器在分享端桌面上的位置，观看端按它把各路摆成对方桌面的样子。
- **输入**：`rcs_input_event.stream` 说明坐标是哪一路画面上的；一个注入器管所有路，按住的按钮拖到另一块显示器上照样按着。单独传送的指针（[08](08-viewing.md#鼠标指针)）的 `rcs_cursor_event.stream` 说明指针在哪一路。
- **观看端选路**：`rcs_view_request_config.streams`（`1u << stream` 位，0：全要）。分享端只采集、编码有观看端要的路；不再有人要或从 `displays[]` 去掉的路结束（两端都收到带 `RCS_STREAM_ENDED` 的 `RCS_EVENT_STREAM`）。
- **出错**：同一块显示器写两次、写了第 0 路的显示器、中间留空、写了没连接的显示器是 `INVALID_ARGUMENT`。共享中的显示器被拔掉：这一路暂停（`DESKTOP`），10 秒后运行失败；跟着 `RCS_EVENT_DISPLAYS` 的宿主应先把它从 `displays[]` 去掉。
- 宿主推的画面和码流（`HOST`、`ENCODED`）只有第 0 路，不能和 `RCS_EXT_STREAMS` 一起用。每路一个采集和一个编码会话：NVENC 在消费级显卡上同时能开的编码会话有限（现在的驱动是 8 个）。

```cpp
// 分享端：主显示器是第 0 路，显示器 2 是第 1 路
auto share = rcsdk::SessionConfig(RCS_ROLE_SHARE).setAddress("0.0.0.0", 47990).setKey(key).setStreams({2});
```

## 宿主自己的画面和声音

画面来源 `RCS_CAPTURE_HOST` 时 SDK 不采集屏幕，宿主推画面；适合引擎渲染的画面、虚拟显示器、应用内共享、白板、自动化测试，以及 iOS、Android（系统只让应用自己采集）。不需要屏幕录制权限。

| 函数 | 画面 | 平台 |
| --- | --- | --- |
| `rcs_session_push_video_frame`（`rcs_frame`） | CPU 上的 BGRA8，尺寸必须是流的尺寸；复制 | 全部 |
| `rcs_session_push_gpu_frame`（`rcsdk/gpu.h`，`rcs_gpu_frame_info`） | 宿主的 GPU 图像，不经 CPU 就编码；尺寸是流的尺寸，`format`、`color` 写明。本系统收哪种看 `rcs_capabilities.pushed_gpu_frames`：macOS、iOS 收 `CVPixelBufferRef`（BGRA8、NV12、P010、NV24、P410，硬件编码要 IOSurface 支撑），Android 收 `AHardwareBuffer*`（BGRA8 代表 8 位 RGB、NV12 代表 YCbCr 4:2:0，推的时候复制）；Windows 现在不收（`UNSUPPORTED`） | macOS、iOS、Android |
| `rcs_session_push_sample_buffer`（`rcsdk/apple.h`） | `CMSampleBuffer` 原样交过来：任意尺寸，SDK 在 GPU 上转正、按原来的形状放进流的尺寸 | macOS、iOS |
| `rcs_session_push_hardware_buffer`（`rcsdk/android.h`） | `AHardwareBuffer` 原样交过来：任意尺寸，按 `rotation` 转正、放进流的尺寸，返回前复制完 | Android |

- 流的尺寸是 `rcs_stream_info.width/height`；观看端要求另一个尺寸时有带 `RCS_STREAM_SIZE` 的 `RCS_EVENT_STREAM`，宿主按新尺寸推（`push_video_frame`、`push_gpu_frame`），或者交给会转正和缩放的两个函数。尺寸不对是 `INVALID_ARGUMENT`。
- 还没有流时（会话刚运行、第一个观看端的流还没开始，或流正在重新开始）返回 `INVALID_STATE`，这一帧不发，接着推下一帧就行。来不及编码时最新的一帧替换上一帧，按流的帧率取用。
- 声音：`rcs_send_audio_config.source = RCS_AUDIO_HOST`，用 `rcs_session_push_audio` 推 48 kHz 立体声交错 float32（[12](12-audio.md)）。
- 输入：`rcs_control_config.target = RCS_INPUT_HOST`，输入只交给 `on_input`（[11](11-input.md)）。
- 宿主推的画面不能和另一个进程里的助手一起用（编码要在会话的进程里）。

```cpp
auto config = rcsdk::SessionConfig(RCS_ROLE_SHARE).setAddress("0.0.0.0", 47990).setKey(key)
                  .setVideo(1280, 720, 60, 8'000'000)
                  .setCaptureSource(RCS_CAPTURE_HOST).setAudioSource(RCS_AUDIO_HOST)
                  .setControl(true, RCS_INPUT_HOST);
rcsdk::Callbacks callbacks;
callbacks.onInput = [&](const rcs_input_event& e) { myScene.handle(e); return true; };
rcsdk::Session session(config, std::move(callbacks));
session.start();
// 渲染线程，每帧（尺寸是流的尺寸）：
session.pushVideoFrame(bgra, 1280, 720, 1280 * 4);
// 音频线程，每个块：
session.pushAudio(samples, frames);
```

## 本地预览

分享端也可以注册 `on_frame` 或 `on_gpu_frame` 看自己发出的画面，但回调在采集编码循环里同步执行，慢了就拖慢采集：不需要预览时不要注册。预览帧在 macOS 上是采集的 YCbCr 布局，Windows 上是 BGRA。另一个进程里的助手不能有本地预览。

## 一对多

一个分享端可以同时服务多个观看端（课堂、演示、远程协助时有人旁观）：`RCS_EXT_VIEWERS` 的 `max_viewers`（0 是默认的 64；也可以设 1..`RCS_MAX_VIEWERS` = 256）。采集只做一份；大家跟得上时编码也只做一份，同一份码流发给每个观看端，有人跟不上时再多编一档给它（见下面的[弱的观看端](#弱的观看端多一档编码rcs_ext_encodings)）。每个观看端有自己的连接、声音（按它协商的格式编码）、数据队列、控制权和统计。TCP、UDP、P2P、中继和 HOST 上都可用（P2P 和中继上每个观看端一次自己的 ICE，见 [09](09-transports.md#p2p-和中继上的多个观看端)；HOST 上每个观看端一条宿主的通道，带宿主的路由号，见 [09](09-transports.md#宿主自己的传输rcs_transport_host)）。人多、分享端的上行撑不住每人一份时经转发器（`RCS_TRANSPORT_FORWARDER`，[09](09-transports.md#经转发器rcs_transport_forwarder)）：每张画面只上传一次，转发器替每个观看端重传、节流、按路径换档。

- **编号**：观看端加入时得到 `rcs_viewer_id`，每次运行从 1 起、不重复使用。和某个观看端有关的事件在 `rcs_event.viewer_id` 里写明是谁；`on_data`、`on_input`、`on_audio` 也带着它。
- **加入**：观看端通过握手后，分享端先看能不能服务它：满员、双方开着声音却没有共同的声音格式时以 `RCS_ERROR_BUSY` 拒绝（原因写明）；解不了主编码的格式或尺寸时，给它一档它能解的低档编码，本端没有它能解的格式（或 `RCS_ENCODINGS_SINGLE`）才 `BUSY`；然后问 `on_viewer_join`。新加入的从它那一档的关键帧开始看；同一档有别人在看时，几个人先后加入的关键帧按 `keyframe_min_interval_ms` 合并成一个，而不是每来一个人所有人吃一个关键帧。
- **主观看端**：最早加入、还在的那个，或宿主用 `rcs_session_set_main_viewer` 指定的。主编码的画质要求和指针模式照主观看端的；`rcs_stream_info.cursor` 告诉每个观看端指针实际在不在画面里。`rcs_session_get_peer_info` / `get_control_info` 是主观看端的。
- **码率**（`bitrate_from`）：每个观看端的路径各自测量。`RCS_BITRATE_FROM_QUORUM`（默认）：主编码取至少 `quorum_percent`（默认 50%）的人达到的最高码率，跟不上的去低档；`RCS_BITRATE_FROM_MINIMUM` 取最低的，人人跟得上、最慢的人决定大家的画质；`RCS_BITRATE_FROM_MAIN` 取主观看端的。低档的码率取它那一档里最低的。决定主编码码率的是谁：`RCS_EVENT_RATE` 的 `viewer_id`、`rcs_session_get_viewer_stats`。
- **控制权**：每个观看端单独。`rcs_control_config.allowed` 是新来者的默认，更新它会设置所有在场的；`rcs_session_set_viewer_control` 改一个。几个观看端可以同时控制，输入按到达顺序执行；有控制权的观看端离开或被收回时，按住的键和按钮全部释放（系统分不清是谁按的）。
- **数据**：`rcs_data_message.viewer_id` 0 发给所有观看端（要么都进队列、要么都不进），编号发给那一个。
- **断开**：`rcs_session_disconnect_viewer` 立即返回（从回调里也能调），随后有 `RCS_EVENT_VIEWER`（离开，`DISCONNECTED`）。`rcs_session_list_viewers` 列出现在连着的。
- **统计**：`rcs_session_get_viewer_stats(session, viewer, section, &stats)` 给一个观看端的 `NETWORK`、`AUDIO_SEND`、`AUDIO_RECEIVE`、`DATA`、`INPUT` 和 `VIEWER_VIDEO`（它在哪一档、发了多少帧、它引起了几个关键帧）；`rcs_session_get_stats` 的这几个分区是所有观看端（含已离开的）的合计，测量值是主观看端的；`RCS_STATS_GROUP` 是整个组的（每档几个人、主编码码率、上行合计、关键帧按原因计数、上下移次数）。
- **开销**：每多一个观看端约 4 个线程、2 个描述符、约 2.5 MB 内存、约 2～4% 的一个核（M4 Pro 上实测），上传是每人一份码流。64 个、每人 2 Mbit/s 时分享端发出约 200 Mbit/s：公网上分享端的上传带宽通常先成为上限。实测表见 [PERFORMANCE](../PERFORMANCE.md)。
- Windows 上一个进程同时只能复制一个显示器一次：要给几个人看同一个显示器，用一次运行加 `max_viewers`，不要开几个会话。

```cpp
auto config = rcsdk::SessionConfig(RCS_ROLE_SHARE).setAddress("0.0.0.0", 47990).setKey(key).setViewers(30);
rcsdk::Callbacks callbacks;
callbacks.onViewerJoin = [](const rcs_viewer_info& viewer, rcs_viewer_decision& decision) {
    decision.control = viewer.viewer_id == 1;   // 只有第一个加入的能控制
};
callbacks.onEvent = [](const rcs_event& e) {
    if (e.type == RCS_EVENT_VIEWER) { /* e.viewer.joined、e.viewer.result、地址、对端 */ }
};
```

### 弱的观看端：多一档编码（`RCS_EXT_ENCODINGS`）

一份码流给所有人时，一个人网差、丢包多、解不了格式，大家的画质和关键帧都会被它拖累。默认（`RCS_ENCODINGS_AUTO`，最多 2 档）分享端在需要时从同一个采集多编一档“低档”给这样的观看端，别人照旧：

- **谁去低档**（`RCS_EVENT_VIEWER_VIDEO`，原因在 `rcs_viewer_video_event.reason`）：它的路径装不下这一档**实际发出的**（不是目标码率：桌面画面大多远低于目标）持续 `move_down_ms`（默认 2 秒），或积压满 1 秒丢了视频（`PATH`，只在 `QUORUM` 和 `MAIN` 下）；一分钟里因它的丢包或积压给同档的人出了 `loss_keyframes_move`（默认 3）个关键帧（`LOSS`）；解不了主编码的格式、画面格式或尺寸（`DECODER`）；宿主指定（`HOST`）。同一档的人过半同时不够时，判为分享端自己的上行到顶了：谁都不动，码率一起降（`rcs_group_stats.uplink_limited`）。
- **回到主编码**：路径持续 `move_up_ms`（默认 10 秒）装得下主编码实际发出的再多 1/4（或达到主编码目标的 95%）。
- **低档是什么**：默认同尺寸、主编码一半的帧率（第 2 档四分之一），宽 × 高 × 帧率不超过 `max_pixel_rate`（默认 1920×1080×30，所以 4K60 的主编码配 1080p30 的低档：放不下时按 75/50/38/25% 缩小）；编码格式是这一档所有观看端都能解的（`lower[].codecs` 可以指定偏好，比如只给只解 H.264 的浏览器）；码率跟着这一档里最慢的人。缩小在 GPU 上做（macOS / iOS 用 VideoToolbox，Windows 用视频处理器，Android 用 GL）。
- **关键帧各档各自的**：一个人加入、丢帧、积压只让它那一档的人吃关键帧；有几个观看端时，同一个人引起的关键帧每 `viewer_keyframe_interval_ms`（默认 2 秒）最多一个，多出来的请求等下一个（`rcs_viewer_video_stats.keyframes_waited`）；只有一个观看端时照旧随要随给。
- **换档**：观看端在新一档的关键帧处换过去，画面不断；它的 `RCS_EVENT_STREAM` 带 `RCS_STREAM_ENCODING`，`rcs_stream_info.encoding` 是它拿的那一档，`encodings` 是分享端现在跑几档，宿主可以据此显示“你的网络较慢：30 帧”。编码格式或尺寸变了时观看端换一个解码器。
- **开销和上限**：每一档一个编码会话，在助手里有自己的线程，不拖慢主编码；没人用了 `lower_idle_ms`（默认 10 秒）就停。系统给不了编码会话时（NVENC 在 GeForce 上整台机器 8～12 个，所有程序一起算）`AUTO` 报告（`RCS_EVENT_ENCODING`，`running` 0、`result` 和原因），那些人留在原来的档，30 秒后再试。
- **模式**：`RCS_ENCODINGS_SINGLE` 一档给所有人（跟不上的人在 `QUORUM` 下退成 `MINIMUM`，在 `MAIN` 下跳关键帧；解不了的 `BUSY`）；`RCS_ENCODINGS_FIXED` 有观看端时 `count` 档一直开着，开不了时运行以原因失败。宿主自己的码流（`RCS_CAPTURE_ENCODED`）只有一档。
- **宿主的决定**：`rcs_session_set_viewer_video(session, viewer, &video)`（加入时在 `rcs_viewer_decision.video` 里就能定）：`encoding` 固定在某一档（`RCS_ENCODING_ANY` 由 SDK 决定），`counts_for_bitrate = 0` 让它“拿路径能拿的、不拖任何人”（比如录屏的观看端，或者课堂里老师的投影要最好的画面时让学生的手机不参与主编码的码率）。
- 每档的编码器统计：`rcs_session_get_encoding_stats(session, stream, encoding, &stats)`（`rcs_video_send_stats`；第 0 档就是 `VIDEO_SEND`）。

```cpp
// 默认就是 QUORUM 50% + 需要时开低档；课堂上可以改成：低档最多 15 帧、只用 H.264
auto config = rcsdk::SessionConfig(RCS_ROLE_SHARE).setAddress("0.0.0.0", 47990).setKey(key).setViewers(50);
auto& encodings = config.encodings();
encodings.lower[0].frames_per_second = 15;
encodings.lower[0].codecs[0] = RCS_CODEC_H264;
rcsdk::Callbacks callbacks;
callbacks.onEvent = [](const rcs_event& e) {
    if (e.type == RCS_EVENT_VIEWER_VIDEO) { /* e.viewer_id 换到 e.viewer_video.encoding，原因 e.viewer_video.reason */ }
};
// 老师的投影要最好的画面：学生的手机不参与主编码的码率（跟不上的自己去低档）
rcs_viewer_video student = RCS_VIEWER_VIDEO_INIT;
student.counts_for_bitrate = 0;
session.setViewerVideo(studentId, student);
```

### 时域分层：先少拿几帧（可选，默认关）

一档编码可以分成几个时域层（`RCS_EXT_ENCODINGS` 的 `scalability`，低档在 `lower[].scalability`；名字照 W3C 的 WebRTC-SVC）：`RCS_SCALABILITY_L1T2` 隔一帧放进上层，`L1T3` 第 0、1 层各四分之一、第 2 层一半。一帧只参考自己这层和更低层的帧，所以：

- **跟不上的观看端先少拿层**：路径装不下它拿的这些层实际发出的、持续 `move_down_ms`，它先只拿低一层（帧率减半），当场生效，不要关键帧、不要恢复，别人不受影响；只拿第 0 层还不够时才去低档编码（有的话），回来时也一层一层回（每步 `move_up_ms`）。原因和换档一样是 `PATH`，`RCS_EVENT_VIEWER_VIDEO` 的 `layers` 是它现在拿几层；观看端的 `rcs_stream_info.layers` 是这一档有几层、`received_layers` 是它拿到几层（变了时 `RCS_EVENT_STREAM` 带 `RCS_STREAM_ENCODING`），宿主可以显示“你的网络较慢：30 帧”。
- **码率**：只拿低层的观看端按“低层装得下它的路径”参与这一档的码率（第 0 层占一半时，3 Mbit/s 的路径能让这一档跑到约 6 Mbit/s），所以 `RCS_ENCODINGS_SINGLE`（只能开一个编码会话时）下一个弱的观看端只拖累别人一半而不是全部。
- **宿主的决定**：`rcs_viewer_video.layers`（0 由 SDK 按路径定；1..3 固定拿这么多层，比如只要半帧率的录屏端）。
- **统计**：`rcs_viewer_video_stats.layers`、`upper_layer_frames`（没发给它的上层帧）、`layer_moves_down` / `layer_moves_up`；`rcs_group_stats` 也有两个层的计数；`rcs_video_send_stats.temporal_layers`（编码器实际做了几层）和 `base_layer_bytes`。
- **哪些编码器能做**（`rcs_codec_implementation.temporal_layers`，编码器做不到时按它能做的，`rcs_codec_event.temporal_layers` 说实际几层）：codec 库 libvpx（VP8、VP9）和 libaom（AV1）3 层；VideoToolbox 的硬件 H.264、HEVC 2 层（4K60 时 H.264 换成流水线编码器就没有层了，HEVC 的流水线编码器仍是 2 层）；NVENC 按驱动报告（最多 3 层）：H.264 驱动 522.25 起，HEVC、AV1 要驱动 570 起（NVENC 接口 13.0；更早的驱动上 NVENC 照常编码这两种，只是不分层，日志里有一行说明）；oneVPL 按运行时接受的（最多 3 层；测试的 PC 的 Intel 核显 H.264、HEVC、AV1 都是 3 层，分层时参考帧失效不用，丢帧从关键帧继续）；Media Foundation 和 MediaCodec 打开后才知道（最多 2 层；MediaCodec 的 HEVC、AV1 最多 3 层）。PC 上试过：NVENC H.264 三层（HEVC、AV1 的PC（RTX 4090，驱动 616.92）上验证过），NVIDIA 的 H.264、AV1 和 Intel 的 H.264、HEVC 的 Media Foundation 编码器两层，Intel 的 VP9 不分层（设得进、编码时失败）；高通的 MediaCodec 编码器（S25+）不分层。
- **代价**：同样的码率下每一帧都差一些。本 SDK 在 M4 Pro 上合成的屏幕内容实测（`tools/rcsdk_encoder_layers`）拿全部层的人同实际码率下低 0.6～7 dB（多数 2～4 dB）、或同画质多花约 1.2～2 倍的码率，只拿第 0 层的人和单独一档半帧率的低档差不多，所以**默认不分层**；只能开一个编码会话、或宁可弱的人掉帧也不愿多开编码会话时再打开。

```cpp
auto& encodings = config.encodings();
encodings.mode = RCS_ENCODINGS_SINGLE;            // 只开一个编码会话
encodings.scalability = RCS_SCALABILITY_L1T2;     // 跟不上的人先拿一半的帧
// 录屏的观看端只要第 0 层
rcs_viewer_video recorder = RCS_VIEWER_VIDEO_INIT;
recorder.layers = 1;
session.setViewerVideo(recorderId, recorder);
```

人多、公网上的大群经转发器（上行不随人数增长，晚来的人从转发器留着的关键帧开始，声音只编一次）：[09](09-transports.md#经转发器rcs_transport_forwarder)、部署见 [24](24-forwarder.md)。还没做的见 [ROADMAP](../ROADMAP.md)。

### 准入：先问用户，再放行

`on_viewer_join` 要马上返回，所以宿主不在回调里等用户，而是返回“之后再决定”（`RCS_ADMIT_LATER`），用户点了以后调用 `rcs_session_admit_viewer`（C++：`Session::admitViewer`），任何线程都行：

```c
static void on_viewer_join(const rcs_viewer_info* viewer, rcs_viewer_decision* d, void* app) {
    d->admission = RCS_ADMIT_LATER;
    show_dialog_async(app, viewer->viewer_id, &viewer->peer);   /* 宿主自己的界面 */
}
/* 用户点了“允许”（界面线程） */
rcs_viewer_decision d = RCS_VIEWER_DECISION_INIT;   /* RCS_ADMIT */
d.control = 1;
rcs_session_admit_viewer(session, viewer_id, &d);
/* 或“拒绝”：d.admission = RCS_REFUSE，d.reason 是告诉对方的原因 */
```

- 问宿主之前握手已经通过：弹窗里可以显示已经证明了的对方（`rcs_viewer_info.peer` 的平台、设备公钥，P2P 和中继上宿主给的路由号）。
- 等待期间连接保持，不发画面和声音、不收输入、不交换模块消息；它占一个 `max_viewers` 的名额，还不在 `rcs_session_list_viewers` 里。
- 两端都有 `RCS_EVENT_ADMISSION`（`rcs_admission_event`）：`WAITING`（`timeout_ms` 是还能等多久，0 不限）、`ADMITTED`、`REFUSED`（`result`、`message`），分享端还有 `GONE`（等待中的观看端走了，之后对它的 `rcs_session_admit_viewer` 是 `INVALID_STATE`）。观看端的状态一直是 `CONNECTING`，宿主据 `WAITING` 显示“等待对方确认…”。
- 被拒绝的观看端以 `RCS_ERROR_REFUSED` 结束，`message` 是分享端宿主给的原因；`rcs_viewers_config.admission_timeout_ms`（默认 60 秒，1 秒到 1 小时，0 不限）内没决定，两端都是 `RCS_ERROR_TIMEOUT`。被拒绝、超时都不重连。
- “先只看、请求控制时再问”是宿主的两步：接受时 `control = 0`，观看端用数据通道或自己的模块请求，宿主再 `rcs_session_set_viewer_control`。

## 暂停分享

`rcs_session_pause(session, RCS_PAUSE_VIDEO)`（或 `RCS_PAUSE_AUDIO`，或两者）：不再采集、编码，不给任何观看端发画面；每个观看端的流结束，并收到 `RCS_EVENT_PAUSE`，`by_peer` 说明是分享端暂停的，观看端可以显示“对方已暂停”而不是黑屏。`rcs_session_resume` 后每个观看端从关键帧开始。连接、输入、数据、模块和另一个方向的声音照常。iOS 上应用进后台时默认自动暂停画面（`RCS_EXT_BACKGROUND`）。

## 观看端走了以后

默认（`rcs_reconnect_config.keep_listening = 1`）最后一个观看端离开后运行回到 `LISTENING`，状态事件带原因，下一个观看端从头协商。观看端或路径造成的失败（P2P 连通检查失败、没有共同格式、违反协议、握手失败）也回到 `LISTENING`；只有分享端自己的失败（采集、编码器、助手、端口）结束运行。设 `keep_listening = 0`（C++ `setKeepListening(false)`）则观看端一走运行就结束，适合一次性的共享。要告诉观看端“我会回来”（程序更新后重启、助手换会话），用 `rcs_session_stop_with_reason(session, RCS_STOP_RESTART)`，观看端会立即重连。见 [09](09-transports.md#断线常驻监听与自动重连)。
