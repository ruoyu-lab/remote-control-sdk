# 08 观看端

观看端连接分享端、要它想要的画质、解码、把画面交给宿主画出来。远程控制见 [11](11-input.md)，声音见 [12](12-audio.md)，传输和重连见 [09](09-transports.md)。

## 向分享端要什么：`rcs_view_request_config`

0 的字段留给分享端决定（默认全是 0）。分享端在它的上限内照办（`rcs_share_video_config.follow_requests` 和 `max_*`，默认不超过它屏幕的像素尺寸和刷新率、200 Mbit/s），超过的按原比例缩小。

| 字段 | 内容 |
| --- | --- |
| `width`、`height` | 尺寸（两个都给或都不给）；要“原生分辨率”就要 `rcs_stream_info.display_width` × `display_height` |
| `frames_per_second`、`bitrate_bps` | 帧率 1..240；码率 128 kbit/s..500 Mbit/s（动态码率时是上限） |
| `codec`、`format` | 要两端都有的另一个编码格式 / 画面格式（必须在本端的 `rcs_decoder_config` 里） |
| `streams` | 要哪几路（`1u << stream` 位，0：全要，[07](07-sharing.md#多显示器)） |
| `audio_codec`、`audio_bitrate_bps` | 分享端到本端的声音的格式和码率（[12](12-audio.md)） |
| `cursor` | `RCS_CURSOR_IN_VIDEO`（默认：指针画进画面）或 `RCS_CURSOR_SEPARATE`（单独传送，见下文） |

运行中随时可以改（`rcs_session_update`），不用重连：

```cpp
// 连接时要 4K 120 FPS，HEVC 优先（两端都有时）
rcsdk::SessionConfig view(RCS_ROLE_VIEW);
view.setAddress(ip, 47990).setKey(key).setCodecs({RCS_CODEC_HEVC, RCS_CODEC_H264})
    .setVideoRequest(3840, 2160, 120, 80'000'000, RCS_CODEC_HEVC);
// 观看中改成 1080p：从会话现在用的要求开始改
auto request = session.settings<rcs_view_request_config>();
request.width = 1920;
request.height = 1080;
session.update(request);
```

实际收到的画面经 `RCS_EVENT_STREAM` 和 `rcs_session_get_stream_info` 报告（`rcs_stream_info`：编码格式、画面格式和颜色、分享端的加速方式、尺寸、帧率、码率、画面在编码帧里的位置 `content_*`、共享的东西的尺寸和刷新率 `display_*`、桌面上的位置、指针模式、声音的格式和码率）；事件的 `changes` 位（`RCS_STREAM_CODEC`、`FORMAT`、`SIZE`、`FRAME_RATE`、`BITRATE`、`CONTENT`、`DISPLAY`、`AUDIO`、`CURSOR`、`ENDED`、`ENCODING`）说明这次变了什么。画质变化时观看端需要时会自动换解码器（例如 H.264 换成 HEVC）。

几个人一起看时，分享端可能把网慢、丢包多或解不了主编码的这一端放到它的一档低档编码上（[07](07-sharing.md#弱的观看端多一档编码rcs_ext_encodings)，比如同尺寸、一半的帧率）：`rcs_stream_info.encoding` 是这一端拿的那一档（0 是主编码），`encodings` 是分享端现在跑几档；换档时有 `RCS_STREAM_ENCODING`，画面从新一档的关键帧接着来，帧率、尺寸、格式可能跟着变。宿主可以据此显示“你的网络较慢，画面 30 帧”。

## 解码：`rcs_decoder_config`

- `codecs[]`：接受的编码格式，默认只有 H.264。列出 HEVC、VP8、VP9、AV1 只是表示接受，用不用由分享端的列表决定。SDK 自带的库在每个系统上都能用 CPU 解 VP8、VP9（libvpx）和 AV1（dav1d），所以用 `SOFTWARE` 或 `HARDWARE_FIRST` 时这三种总能列上。
- `formats[]`：宿主的渲染器能画的画面格式，默认只有 8 位 4:2:0。**只有列出的才会收到**：10 位的 GPU 帧是 P010，4:4:4 是 NV24 / P410，HDR10 的值是 PQ 编码的，渲染器不认识就会画错（[13 画面格式](13-tuning.md#画面格式)）。
- `acceleration`：`HARDWARE`（默认，没有硬件解码器时 `start` 以 `UNSUPPORTED` 失败）、`SOFTWARE`，或 `HARDWARE_FIRST`：每路流开始时，硬件能解它的编码格式和画面格式就用硬件，不能就用软件（例如 Windows 的 HEVC 4:4:4：NVIDIA 显卡上由 NVDEC、Intel 显卡上由 oneVPL 在 GPU 上解，别的显卡上只有软件解码器解；Mac 的硬件不解 VP9 4:4:4）；选了哪个由 `RCS_EVENT_CODEC` 报告，解码器失败时不会换成另一个。
- `backend`：`RCS_BACKEND_AUTO`（默认：硬件排在前面；软件里 SDK 自带的库排在系统的前面，H.264 除外）或指定一个（`RCS_BACKEND_LIBVPX`、`DAV1D`、`OPENH264` 是自带的库）。
- 解码队列（`queue_frames`、`max_queue_age_ms`）是调优（[13](13-tuning.md)），在宿主的 D3D11 设备上解码是 `RCS_EXT_GPU_DEVICE`（[04](04-quickstart-windows.md#观看端用-d3d11-显示画面)）。
- 解码器出错时（例如休眠唤醒、GPU 重置）观看端换一个新解码器，从下一个关键帧继续（`RCS_EVENT_RECOVERY`，`RCS_RECOVERY_DECODER`）；恢复后还没解出一帧又出错，本次运行以解码器的错误结束。

## 画面交给宿主

`on_gpu_frame` 和 `on_frame` 只能注册一个（都注册时 `rcs_session_create` 返回 `INVALID_ARGUMENT`），SDK 不在两者之间自动分流：

- **`on_gpu_frame`**：原生 GPU 表面，宿主在 GPU 上转换和显示，几乎不花时间。`rcs_gpu_frame_get_info` 给出资源类型、布局（`format`：解码帧默认 NV12，其他画面格式是 P010、NV24、P410；Windows 的软件解码是 BGRA8 纹理）、值的含义（`color`：原色、传递函数、YCbCr 矩阵和范围）、逻辑尺寸和表面尺寸（表面可能更大，只画逻辑尺寸那部分）。

  | 系统 | `resource` | 画法 |
  | --- | --- | --- |
  | macOS、iOS | `CVPixelBufferRef`（IOSurface 支撑） | `CVMetalTextureCache` 零复制（[03](03-quickstart-macos.md#观看端用-metal-显示画面)） |
  | Windows | `ID3D11Texture2D*`，`device` 是它的 `ID3D11Device*` | 两个着色器视图或视频处理器（[04](04-quickstart-windows.md#观看端用-d3d11-显示画面)） |
  | Android | `AHardwareBuffer*` | `EGLImage` + `GL_TEXTURE_EXTERNAL_OES`，或 Vulkan 导入（[06](06-quickstart-android.md#观看端)） |

  帧借给回调；要留到回调之后（交给渲染线程）就 `rcs_gpu_frame_retain`，用完 `rcs_gpu_frame_release`（C++ 复制 `rcsdk::GpuFrame` 即可）。留住的帧在会话停止后仍然有效。需要在 CPU 上等 GPU 写完时调 `rcs_gpu_frame_wait`；在同一个设备上渲染一般不需要等。
- **`on_frame`**：CPU 上的 BGRA8 图像（`rcs_frame`，只在回调期间有效），帧在 GPU 上时由 SDK 读回，这是宿主明确选择的代价。任何 SDR 格式都由 SDK 转好；HDR10 和 `on_frame` 一起是 `UNSUPPORTED`。
- 都不注册：不交付画面（例如只录不看的观看端，[17](17-replacing.md#编码包交给宿主录制转发审计)）。
- `rcs_capabilities.software_decoding_gpu_frames` 说软件解码能不能也交出 GPU 帧（macOS、iOS、Android 是流本来的布局，Windows 是上传的 BGRA8 纹理）。

### 怎样画得快

屏幕到屏幕的延迟多半在这里（实测见 [PERFORMANCE](../PERFORMANCE.md) “屏幕到屏幕，与 Moonlight 对比”）：

- **帧到了就画**：回调在 SDK 的线程上，直接让渲染线程画这一帧，不要先排到界面线程、等下一次界面同步。QML 示例的做法：形状不变的帧给 Qt Quick 的渲染线程投一个任务（`QQuickWindow::scheduleRenderJob`），在那里 `update()`，渲染节点画的时候取最新的一帧（`examples/qml-demo/remote_session.cpp`、`remote_video.cpp`）。正在被 GPU 读取的前几帧要继续持有（示例保留 3 帧），否则解码器可能复用它们的表面。
- **画完立即提交，不等显示器刷新**：窗口里的画面本来就由系统合成器按刷新同步，不会撕裂；应用自己再等 vsync，帧率和屏幕刷新率相同时（120 FPS 的流在 120 Hz 屏上）可绘制缓冲会排满两三帧，实测延迟翻倍。Qt 是 `QSurfaceFormat::setSwapInterval(0)`，Metal 是 `CAMetalLayer.displaySyncEnabled = NO`，DXGI 是 `Present(0, …)`，EGL 是 `eglSwapInterval(…, 0)`。全屏独占显示时（不经合成器）才需要权衡撕裂。
- **流的帧率等于屏幕刷新率时的取舍**：SDK 交出的帧本身是均匀的，但带着网络抖动，偶尔两帧落进同一次刷新、只显示后一帧（立即提交时 120 Hz 屏上约 106～112 个不同画面 / 秒；等刷新、排一帧约 114～116 个，但慢 3～9 ms）。两者都要的宿主可以每次刷新最多提交一帧：同一次刷新里已经提交过一帧时，把新到的推到下一次刷新。“哪一次刷新”要按系统给的刷新时刻算（macOS 的 `CADisplayLink`、Windows 的 `DwmGetCompositionTimingInfo` / 可等待交换链、Android 的 `Choreographer`）；只按刷新周期让两次提交至少隔一个周期试过，更差。

## 画面在窗口里的位置和坐标

分享端把共享的东西按原比例放进编码帧，多余的部分是黑边：`rcs_stream_info.content_x/y/width/height` 是画面在编码帧里的位置。`rcsdk/view_geometry.hpp`（只有头文件，可选）做窗口坐标、编码帧和共享的显示器之间的换算：

- `rcsdk::viewGeometry(viewportWidth, viewportHeight, frameWidth, frameHeight, ViewScale::fit / nativePixels, panX, panY)`：画面在窗口里放在哪里（适应窗口加黑边，或 1:1 加平移）。画和输入必须用同一个几何。
- `ViewGeometry::position(px, py, frameWidth, frameHeight, x, y)`：窗口像素 → 编码帧上的 0..65535；`displayPosition(x, y, streamInfo)`：再去掉编码时加的黑边，得到远程输入要的坐标（点在黑边上返回 `false`）。反方向是 `framePosition` 和 `ViewGeometry::point`。
- `displayScale(geometry, streamInfo)`：窗口像素和分享端显示器像素之比，用来缩放单独传送的指针图像。

```cpp
#include <rcsdk/view_geometry.hpp>

// 窗口里的一次点击变成远程输入（分享端的 0..65535）
bool clickToInput(double px, double py, int viewW, int viewH, const rcs_stream_info& s, rcs_input_event& e) {
    const auto g = rcsdk::viewGeometry(viewW, viewH, s.width, s.height);   // 和画画面时用的同一个
    uint32_t x = 0, y = 0;
    if (!g.position(px, py, s.width, s.height, x, y) || !rcsdk::displayPosition(x, y, s)) {
        return false;   // 点在画面外或黑边上
    }
    e.x = x;
    e.y = y;
    e.stream = s.stream;
    return true;
}
```

## 鼠标指针

默认指针和桌面一起画进画面，宿主什么都不用做。观看端设 `rcs_view_request_config.cursor = RCS_CURSOR_SEPARATE`（C++ `setCursorMode`）后：

- 分享端不再把指针画进画面，只移动指针也不再产生新画面，省下码率。
- 观看端收到 `RCS_EVENT_CURSOR`（`rcs_cursor_event`）：是否显示、在哪一路、热点位置（和输入一样是 0..65535 的显示器坐标）、形状编号；某个形状第一次出现时附带图像（`rcs_cursor_shape`：BGRA8 直通 alpha、热点、宽高，单位是分享端显示器的像素）和它是哪种标准指针（`type`：箭头、文本、小手、等待、调整大小……，应用自定义的是 `RCS_CURSOR_CUSTOM`）。同一图像在本次运行中编号不变，宿主按编号缓存最近给它的 `RCS_MAX_CURSOR_SHAPES`（32）张，先给的先丢：一张图像在它之后又给过 32 张别的再出现时，事件会再附带一次图像，所以宿主不用、也不应该保留更多（恶意的分享端可以不停发新形状）。
- 怎么画由宿主决定：控制时把本机鼠标指针设成远端的形状（本机指针跟手，没有延迟）；只看时在画面上按位置叠加画出；也可以按 `type` 换成自己的图标。位置用 `framePosition` + `ViewGeometry::point` 换算，大小用 `displayScale`。
- 运行中可以切换模式。一对多时画面只有一个，照主观看端的模式（`rcs_stream_info.cursor` 说实际的）。示例 `examples/qml-demo` 用的就是单独传送。

## 几路画面

分享端发几路时（[07](07-sharing.md#多显示器)），每一路有自己的 `RCS_EVENT_STREAM`（`stream_index`）、帧（`rcs_frame.stream`、`rcs_gpu_frame_info.stream`）、统计（`rcs_session_get_stream_stats`）。`rcs_stream_info.desktop_*` 是每块显示器在分享端桌面上的位置，观看端按它把各路拼成对方桌面的样子，或者放进各自的窗口、标签。

```cpp
callbacks.onFrame = [&](const rcs_frame& f) { windows[f.stream].show(f); };
callbacks.onEvent = [&](const rcs_event& e) {
    if (e.type == RCS_EVENT_STREAM) {
        const auto& info = e.stream.info;   // info.stream == e.stream_index
        if (e.stream.changes & RCS_STREAM_ENDED) {
            windows[e.stream_index].close();
        } else {
            windows[e.stream_index].place(info.desktop_x, info.desktop_y, info.desktop_width, info.desktop_height);
        }
    }
};
```

## 暂停

窗口到了后台、被挡住、最小化时，观看端可以暂停画面或声音，连接、输入、数据、模块和另一个方向的声音照常：

```c
rcs_session_pause(session, RCS_PAUSE_VIDEO);    /* 这个窗口看不见了 */
rcs_session_resume(session, RCS_PAUSE_VIDEO);   /* 又露出来了 */
```

- 暂停画面：本端立刻不再解码、关掉解码器；分享端不再给这个观看端发画面，它的流在本端结束（`RCS_EVENT_STREAM` 带 `RCS_STREAM_ENDED`），没有观看端要的流分享端也不再采集和编码。恢复后分享端重新公告，从关键帧开始。
- 暂停声音：本端停止播放，分享端不再发声音。
- `RCS_EVENT_PAUSE`（`rcs_pause_event`）报告每次变化：`paused` 是现在暂停着的（合起来），`by_host`（宿主暂停的）、`by_background`（应用进后台自动暂停的，iOS）、`by_peer`（分享端暂停了，可以显示“对方已暂停”）各记各的。宿主的恢复只取消宿主自己的暂停。任何状态都能调用：没在运行的会话以暂停的状态开始。

## 断线重连

默认（`rcs_reconnect_config.reconnect = 1`）运行中路径断了，观看端进入 `RECONNECTING`，每 `reconnect_interval_ms`（默认 1 秒）连一次，直到连上或 `reconnect_window_ms`（默认 30 秒）用完；连上后画面从关键帧重新开始，设置和要求照旧。分享端正常关闭了连接（`RCS_ERROR_DISCONNECTED`）时不重连；它说会回来（`RCS_ERROR_RESTARTING`）时立即重连；第一次就连不上也不重试。详见 [09](09-transports.md#断线常驻监听与自动重连)。
