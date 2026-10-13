# 画面格式：各平台能做什么

怎样选画面格式（4:2:0 / 4:4:4、8 / 10 位、SDR / HDR10，矩阵和范围）、两端怎样协商、观看端收到什么，见开发者指南的[画面格式](guide/13-tuning.md#画面格式)。本页是参考：每个平台、每个编解码后端实际能编、能解哪些格式，做不到的和原因。接口以 `include/rcsdk/rcsdk.h` 为准（`rcs_picture_format`、`rcs_color`、`rcs_codec_implementation.picture_formats`、`rcs_capabilities.picture_formats`）。

**本机能做什么，以运行时的查询为准**：`rcs_list_codec_implementations` 给出每个编解码实现的 `picture_formats`，`rcs_get_capabilities` 的 `picture_formats` 是分享端的采集能产生的格式；`rcsdk_cli --info` 把它们打印出来。下面的表是在列出的设备上测出来的，别的设备可能不同。SDK 不靠型号表判断：编码器按它自己报告的档次和输入类型，解码器按能不能解出一组内置的探测码流（Apple 的系统和 Android 都只说解哪种编码格式，不说解哪些档次）。接受了格式设置却悄悄出 4:2:0 或 8 位的编码器，会被每个关键帧的参数集检查发现，以 `RCS_ERROR_CODEC` 结束，不会悄悄降级。

**HDR10**（`RCS_PICTURE_HDR10_420`、`RCS_PICTURE_HDR10_444`）只有接口和协商，还没有任何实现报告它；SDK 自带的 libvpx（VP9）、libaom 和 dav1d 报告它（它们能编解 BT.2020 PQ 的 10 位），但还没有哪个系统的采集产生 HDR10 的画面。

**SDK 自带的编解码库**（每个系统相同，软件）：

| 库 | 编码 | 解码 |
| --- | --- | --- |
| libvpx（VP8） | 4:2:0 8 位 | 4:2:0 8 位 |
| libvpx（VP9） | 全部六种（profile 0～3） | 全部六种 |
| libaom（AV1，只编码） | 全部六种（Main：4:2:0；High：4:4:4） | — |
| dav1d（AV1，只解码） | — | 全部六种 |
| OpenH264（H.264，默认关；没有平台层的系统上默认开） | 4:2:0 8 位 | 4:2:0 8 位 |

它们在 CPU 上处理 YCbCr：采集的画面在分享端转换成流的布局（macOS 原地读像素缓冲，Windows 在 GPU 上转换后读回，Android 和宿主的 BGRA 图像在 CPU 上转），解出的画面交成本系统的帧（macOS / iOS 的像素缓冲，Windows 的 NV12 / P010 纹理、4:4:4 是 BGRA8 纹理，Android 的硬件缓冲区；没有的系统是 BGRA 的 CPU 帧）。

## 分享端：采集

| 平台 | 采集能产生的格式（`rcs_capabilities.picture_formats`） |
| --- | --- |
| macOS | SDR 的四种：4:2:0 / 4:4:4 × 8 / 10 位。ScreenCaptureKit 直接给出编码器要的布局（`420v`、`420f`、`x420`、`xf20`、`444v`、`444f`、`x444`、`xf44`），不另做转换 |
| Windows | SDR 的四种。采集画 BGRA8，GPU 上转成编码器要的布局：NV12 / P010 由视频处理器转，AYUV / Y410 由 SDK 自己的着色器写（RTX 4090 的视频处理器不写这两种） |
| iOS / iPadOS | SDR 的四种：宿主推的画面（`CVPixelBuffer`、sample buffer）按编码器要的布局转换 |
| Android | 只有 4:2:0 8 位 |

## 编码

**macOS**（Apple M4 Pro，macOS 26.5.1 实测）：

| 格式 | 低延迟编码器（SDK 的首选） | 流水线硬件编码器 | 软件 |
| --- | --- | --- | --- |
| H.264 4:2:0 8 位 | 是 | 是 | 是 |
| H.264 4:4:4 8 位（High 4:4:4 Predictive） | 否（悄悄出 4:2:0，所以 SDK 改用流水线编码器，统计里 `low_latency` 为 0） | 是 | 否 |
| H.264 10 位 | 否 | 否（硬件编码器没有 High 10） | 否 |
| HEVC 4:2:0 8 位、10 位（Main、Main 10） | 是 | 是 | 不实时，SDK 不列 |
| HEVC 4:4:4 8 位、10 位（Main 4:4:4、Main 4:4:4 10） | 是 | 是 | 否 |
| VP8、VP9、AV1 | Mac 的硬件不编码，用 SDK 自带的库（上表） | | |

**Windows**（RTX 4090 和 Intel 核显，同一台 PC 实测；oneVPL 一列的 Intel 驱动 32.0.101.8860）：

| 格式 | NVENC（RTX 4090） | Media Foundation：NVIDIA 的编码器 | Media Foundation：Intel 的编码器 | oneVPL（Intel 核显） |
| --- | --- | --- | --- | --- |
| 4:2:0 8 位 | H.264、HEVC、AV1 | H.264、HEVC、AV1 | H.264、HEVC | H.264、HEVC、AV1 |
| 4:2:0 10 位 | HEVC、AV1 | HEVC（AV1 否：拒绝 P010 纹理） | HEVC | HEVC、AV1 |
| 4:4:4 8 位 | H.264、HEVC（AV1 否：这代 GPU 的 AV1 只有 4:2:0） | HEVC（H.264 否：拒绝 High 4:4:4 档次） | 否 | HEVC |
| 4:4:4 10 位 | HEVC（SDK 交 BGRA，NVENC 按码流的矩阵和范围自己转换） | HEVC | 否 | HEVC |

Intel 核显的 Media Foundation VP9 编码器（同一台 PC）：4:2:0 8 位。VP8 硬件编码器测试的 PC 上没有。oneVPL 的运行时也列出 VP9（profile 0～3），但它的 VP9 编码器不按要求出关键帧（同一驱动），SDK 不提供；测试的 PC 上 Windows 也没有登记 Intel 的 AV1 编码器，Intel 的 AV1 只经 oneVPL。

NVENC 按每块 GPU 的能力位报告格式，Media Foundation 的编码器按它登记的输入类型（P010 → 10 位，AYUV → 4:4:4，Y410 → 4:4:4 10 位），oneVPL 按运行时的实现描述里 Direct3D 11 纹理的档次和输入格式（同上的对应，4:4:4 是 HEVC 的范围扩展档次）。AMD 的编码器走 Media Foundation 的规则，没有实测。

**iOS / iPadOS**：和 macOS 同样的规则（按 VideoToolbox 报告的档次）。iPad 真机还没有验证；模拟器没有硬件编码器。

**Android**：MediaCodec 只编码 4:2:0 8 位，别的格式是 `UNSUPPORTED`（“Android encodes 4:2:0 8-bit pictures”）；SDK 自带的库照上表。S25+ 的高通芯片只有 H.264 和 HEVC 的硬件编码器（实测，没有 VP8、VP9、AV1 的）。


## 解码

**macOS**（Apple M4 Pro）：硬件 / 软件。

| 格式 | H.264 | HEVC | VP9（硬件） | AV1（M3 起硬件） |
| --- | --- | --- | --- | --- |
| 4:2:0 8 位 | 是 / 是 | 是 / 是 | 是 | 是 / 否 |
| 4:2:0 10 位 | 是 / 否 | 是 / 是 | 是 | 是 / 否 |
| 4:4:4 8 位 | 是 / 否 | 是 / 是 | 否 | 否 / 否 |
| 4:4:4 10 位 | 是 / 否 | 是 / 是 | 否 | 否 / 否 |

VP9 的硬件解码是苹果的补充解码器（SDK 向系统申请：macOS 11、iOS 26.2 起）。VideoToolbox 不在 CPU 上解 VP9 和 AV1，也不解 VP8：这些用 SDK 自带的库。

**Windows**（Windows 自带的 Media Foundation 解码器，HEVC、VP8、VP9、AV1 来自视频扩展；NVIDIA GPU 上另有 NVDEC，`RCS_BACKEND_NVDEC`；Intel GPU 上另有 oneVPL，`RCS_BACKEND_ONEVPL`）：

| 格式 | 硬件（GPU） | 软件（CPU） |
| --- | --- | --- |
| H.264 | 4:2:0 8 位 | 4:2:0 8 位 |
| HEVC | 4:2:0 8 位、10 位；4:4:4 8 位、10 位经 NVDEC 或 oneVPL（GPU 报告它解时；RTX 4090、Intel 核显实测） | 4:2:0 8 位、10 位，4:4:4 8 位、10 位 |
| VP8 | 4:2:0 8 位（GPU 有 VP8 档次时） | 4:2:0 8 位 |
| VP9 | 4:2:0 8 位、10 位（profile 0、2）；4:4:4 8 位、10 位（profile 1、3）经 oneVPL（Intel 核显实测） | 4:2:0 8 位，其余按探测码流（测试的 PC：10 位） |
| AV1 | 4:2:0 8 位、10 位（4:4:4 经 oneVPL，GPU 有 AV1 High 时；这块核显没有） | 4:2:0 8 位、10 位 |

NVDEC 只接 Media Foundation 在 GPU 上解不了的 4:4:4（它按 `cuvidGetDecoderCaps` 报告每块 NVIDIA GPU 解哪些编码格式的 4:4:4；RTX 4090 只有 HEVC），实现列表里排在 Media Foundation 的硬件解码器之后。oneVPL 的解码器按运行时的实现描述报告（档次加 D3D11 纹理的输出格式：NV12 → 4:2:0 8 位，P010 → 10 位，范围扩展、VP9 profile 1 / 3、AV1 High 的 AYUV → 4:4:4，Y410 → 4:4:4 10 位），排在 NVDEC 之后；4:2:0 它也解（指定 `RCS_BACKEND_ONEVPL` 时），但 `AUTO` 把 4:2:0 留给 Media Foundation（同一块核显上快 0.5～3 ms），只在 Windows 没装这种编码格式的解码器时交给它。两者 4:4:4 的画面都和软件解码器的一样是 BGRA8 纹理（Direct3D 11 没有宿主能采样的 4:4:4 YCbCr 格式），由 SDK 的着色器按流的矩阵和范围在 GPU 上转换；oneVPL 的 4:2:0 画面是 NV12 / P010 纹理，和 Media Foundation 的一样。

要 4:4:4 的 Windows 观看端设 `RCS_ACCELERATION_HARDWARE_FIRST`（或 `backend` 设 `RCS_BACKEND_NVDEC`、`RCS_BACKEND_ONEVPL`）：NVIDIA GPU 上 4:4:4 的流由 NVDEC 在 GPU 上解（1080p 每帧约 3～5 ms，4K 约 6～8 ms），Intel GPU 上由 oneVPL 解（这块核显 1080p 约 6 ms，4K 约 15.5 ms），别的 GPU（AMD）上由软件解码器解、上传成 BGRA8 纹理交给 `on_gpu_frame`（同一台 PC 上滚动的彩色文字 1080p 每帧约 35～38 ms、要 3 个核，见 [PERFORMANCE](PERFORMANCE.md) 的“Windows：Intel 核显上 oneVPL 解码，解码器的纹理池”）；4:2:0 的流仍由 Media Foundation 在 GPU 上解。只用 `HARDWARE` 加 `AUTO` 时报价按实现列表里第一个硬件解码器（Media Foundation 的，4:2:0），不报 4:4:4。双显卡的机器上 `HARDWARE_FIRST` 不看宿主的 `d3d11_device` 在哪块 GPU 上：设备所在的 GPU 不解、另一块解的格式开始解码时以 `UNSUPPORTED` 失败（例如设备在 NVIDIA GPU 上、流是 VP9 4:4:4）。

**iOS / iPadOS**：和 macOS 同样的探测（解得出哪些探测码流就报哪些）。iPad 真机还没有验证；模拟器里只有软件解码。

**Android**：4:2:0 8 位；其余（H.264、HEVC、VP9、AV1 的 10 位和 4:4:4）按解不解得出探测码流、解出的画面能不能正确显示（模拟器的 GPU 画不出 10 位的缓冲区，模拟器上就不报 10 位）。Samsung Galaxy S25+（Android 16 实测）：HEVC、VP9、AV1 的硬件解码器解 10 位（VP9、AV1 解 libvpx、libaom 的码流），高通的解码器不解 4:4:4；VP8 只有 Android 自己的软件解码器（`c2.android.vp8.decoder`）。从 GPU 读回 YCbCr 的画面（交给宿主的 CPU 帧）按流的色彩转换，不按解码器给缓冲区标的（GPU 有 `GL_EXT_YUV_target` 时；高通的有）：Android 的色彩空间（data space）表达不了 BT.709 原色配 BT.601 矩阵（屏幕画面、JPEG 都是这样），高通的 AV1 硬件解码器就把它标成 BT.709，按它标的读回会偏约 15 级。宿主自己在 GPU 上采样解码器的硬件缓冲区时，也要按 `rcs_gpu_frame_info.color` 转换（例如 Vulkan 的 YCbCr 转换指定矩阵和范围），不要按缓冲区标的。

## 做不到的（`UNSUPPORTED` 和原因）

| 情况 | 原因 | 以后怎样做到 |
| --- | --- | --- |
| Windows 在 AMD 的 GPU 上解 HEVC 4:4:4 | Media Foundation 的 HEVC 解码器在 GPU 上收下 4:4:4 画面却不交出（RTX 4090 和 Intel 核显都一样），尽管两块 GPU 都有 HEVC 范围扩展的 D3D11 解码档次。NVIDIA GPU 上 NVDEC 解（`RCS_BACKEND_NVDEC`），Intel GPU 上 oneVPL 解（`RCS_BACKEND_ONEVPL`） | 通用的：自己解析码流、填 DXVA 的图像参数交给 `ID3D11VideoDecoder`（见 [ROADMAP](ROADMAP.md#通用化还缺的能力)）；或宿主自己的解码器（`RCS_EXT_HOST_DECODER` 可以声明 4:4:4） |
| Windows 解 H.264 4:4:4 | 微软的解码器不解，两块 GPU 都没有这个档次 | 宿主自己的解码器 |
| AV1 4:4:4 的硬件编解码 | 这代 NVENC 不编码；M4 Pro 不解 AV1 High；测试的 PC 的 GPU 也没有 AV1 High 的解码档次（oneVPL 的实现描述里核显也没有） | 用 SDK 自带的 libaom 和 dav1d（`HARDWARE_FIRST` 或 `SOFTWARE`） |
| NVIDIA 的 Media Foundation AV1 编码器 10 位 | 拒绝 P010 纹理 | NVIDIA 上用 NVENC（默认后端）编 AV1 10 位 |
| Media Foundation 编码器的 H.264 4:4:4，Intel 的 4:4:4 | NVIDIA 的拒绝档次，Intel 的不收 4:4:4 输入 | NVIDIA 上用 NVENC；Intel 的 HEVC 4:4:4 经 oneVPL（`RCS_BACKEND_ONEVPL`，Intel 核显上默认）；Intel 的 H.264 4:4:4 硬件不编 |
| macOS 编码 H.264 10 位 | 硬件编码器没有 High 10 档次 | — |
| macOS 软件编码 4:4:4、10 位 | 软件编码器只有 4:2:0 8 位 | — |
| Android 编码 10 位、4:4:4 | SDK 只配置 4:2:0 8 位的 MediaCodec 编码器 | — |
| 高通的解码器解 4:4:4 | 解码器不解（S25+） | — |
| HDR 的 CPU 帧 | BGRA8 装不下 HDR，转换就是色调映射 | 新的 CPU 帧格式（例如 RGBA16F） |
| HDR10（任何平台） | 还没有实现：采集、交给宿主编码器的格式、写进码流的 HDR 元数据都没有做 | 补上这三样（`rcs_picture_format` 已经有 HDR10 的值） |

接口为什么这样设计见 [API-DESIGN](API-DESIGN.md#4-决定) 第 32、33 条。
