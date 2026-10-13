# 测量和维护用的工具

这里的程序用于性能测量和维护 SDK，**不随 SDK 发布，不是 SDK 的一部分**。`cmake --preset release` 在 macOS 和 Windows 上编译它们（`RCSDK_BUILD_TOOLS`，作为子项目时默认关；iOS 不编，Android 要另外打开，见下文）。测量的方法和结果见 [docs/PERFORMANCE.md](../docs/PERFORMANCE.md)。

| 程序 | 平台 | 用途 |
| --- | --- | --- |
| `rcsdk_latency_target` | macOS、Windows | 在被控端运行的延迟目标：全屏显示可识别的标记和一个持续移动的色块，按空格切换标记 |
| `rcsdk_response_probe` | macOS、Windows（Android 可选） | 在观看端运行：经 SDK 发送空格，测量从发出按键到解码出新画面的时间（终点是 SDK 交付解码帧，不含绘制和显示器），并统计帧率、丢包、恢复次数和解码耗时 |
| `rcsdk_glass_probe` | macOS | 屏幕到屏幕的画面延迟，不经过 SDK（所以也能测 Moonlight 等其他软件） |
| `rcsdk_audio_latency` | macOS、Windows | 声音的延迟：同一时钟下听到原声和经 SDK 播出的声音 |
| `rcsdk_videotoolbox_probe` | macOS | **维护工具**：列出 VideoToolbox 每个编码器的档次，按每种模式真编码、解析 SPS、用硬件和软件解码器解回来；`--streams` 打印 SDK 的解码探针码流（`src/platform/decode_probes.hpp` 由它重新生成）。它用了 SDK 的内部头文件（`src/`），不是宿主的例子 |
| `rcsdk_codec_probes` | 编进 libvpx 和 libaom 时 | **维护工具**：打印 `src/platform/decode_probes.hpp` 里 VP9 和 AV1 的探针关键帧（每种画面格式一张中灰的画面，用 SDK 自带的 libvpx、libaom 编出，带包的配置：`vpcC`、序列头），系统不报告自己能解哪些格式时，SDK 用它们试解码；它用了 SDK 的内部头文件 |
| `rcsdk_encoder_layers` | macOS、Windows、Android（另配带工具和测试的目录） | **测量工具**：每个编码器做时域分层（L1T2、L1T3）的代价和第二个编码会话对第一个的影响，见下文；它用了 SDK 的内部头文件 |
| `rcsdk_codec_timing` | macOS、Windows | **测量工具**：一种编码格式和画面格式下，这台机器每个编码器、解码器实时每帧花多少时间和 CPU，见下文；它用了 SDK 的内部头文件 |
| `rcsdk_forwarder_load` | macOS、Linux（另配带工具的目录），编转发器时才有 | **测量工具**：给转发器（`rcs_forwarder`）加负载：合成的分享端和许多轻量的观看端，量每个观看端收到的帧率、画面延迟、丢失和修复，以及转发器进程的 CPU、内存和出口流量，见下文；它用了 SDK 的内部头文件 |
| `visual_marker.hpp` | — | 延迟目标、探针和 `rcsdk_cli --pattern-marker` 共用的标记的画法和识别 |

## 按键到画面

```sh
# 被控端（需要能接受远程输入，并保持目标窗口在前台）
rcsdk_cli --role share --address <本机地址> --key-file key.bin --allow-control &
rcsdk_latency_target 40
# 观看端
rcsdk_response_probe --address <被控端地址> --key-file key.bin --samples 30 --seconds 20 --report response.csv
```

- `rcsdk_latency_target [秒数=60]`；Windows 版还有 `[显示器=0] [noise] [burst] [clock=IP:PORT]`：`noise` 在标记周围填满每帧变化的随机色块（压满码率）；`burst` 让画面静止 1.5 秒、每帧变化 0.5 秒，循环；`clock=` 见下面的屏幕到屏幕。
- 探针默认提供 HEVC 和 H.264，由分享端的列表决定用哪个；`--codec`、`--acceleration` 可以改。`--transport host --relay 地址:端口 [--relay-datagram]` 时经 `rcsdk_host_relay` 测。探针每帧要从 GPU 读回几行像素，Windows 上会让接收帧率下降约 3～4 FPS；只测吞吐时用 `--samples 0`，或者直接用 `rcsdk_cli --role view` 读统计。
- **不经屏幕采集的目标**：分享端 `rcsdk_cli --host-pattern --pattern-marker --allow-control --host-input`（再加 `--audio off` 免得要采集声音）推的画面里就有探针认的标记，观看端的空格让它前进。锁屏的 Mac、没有显示器的机器、手机上的原生程序都能这样测“按键→编码→网络→解码”。探针在 Apple 和 Windows 以外的系统用 CPU 帧读标记。Android 版要另配一个带工具的目录：`cmake --preset android-arm64 -B build/android-tools -DRCSDK_BUILD_TOOLS=ON -DBUILD_TESTING=OFF`，再 `adb push` 到 `/data/local/tmp`。

## 屏幕到屏幕

被控端运行 `rcsdk_latency_target <秒数> 0 clock=<Mac 地址>:47911`（Windows），标记改为显示与 Mac 对时后的毫秒数，每帧重画；Mac 上 `rcsdk_glass_probe --pid <观看程序的进程号> [--seconds 20] [--report latency.csv]` 同时提供 UDP 对时，并用 ScreenCaptureKit 抓取观看程序在屏幕上合成后的画面，用每帧的上屏时间减去标记。结果包含被控端合成、采集、编码、网络、解码和 Mac 端合成，不含显示器扫描。观看窗口必须在当前桌面空间里。

## 声音延迟

- macOS：`rcsdk_audio_latency [--capture tap|sck] [--capture-period N] [--playback-period N] [--margin-us N] [--codec C]`：录音器是一个包含本进程在内的 Core Audio 进程捕获，咔哒声由子进程 `afplay` 播放，间隔 1.0137 秒（落在采集和输出周期的不同位置）。
- Windows：`rcsdk_audio_latency <播放咔哒声的计划任务名> [秒数] [opus|aac|pcm]`：在同一进程里运行分享端、观看端和一个环回录音器，另一个进程（计划任务）播放咔哒声；两者之差就是 SDK 到系统混音的声音延迟（不含扬声器）。

## 时域分层和第二个编码会话

`rcsdk_encoder_layers` 回答两件事（结果用在[指南 07 的时域分层](../docs/guide/07-sharing.md#时域分层先少拿几帧可选默认关)）：每个编码器分层要付出多少画质，第二个编码会话（低档编码）会不会拖慢主编码。它对这台机器上每个编码器（编码格式 × 硬件或软件 × 后端；Windows 上每块 GPU 各一遍）做：

1. **分层**：同样的画面按 `--fps` 的时间戳尽快编码，每个目标码率各编一遍 L1T1、L1T2、编码器做得到的话再一遍 L1T3，并且各用一个观看端解：
   - `kbit/s`、`pictures`、`keys`：实际码率（含关键帧）、编出的帧、关键帧数；
   - `coded`：编出来的帧的亮度 PSNR；`shown`：每一帧都按显示算（没编出的帧显示上一帧），对观看者有意义的是这个；
   - `base%`：第 0 层占的码率（不算关键帧）；`base alone`：只拿第 0 层的观看端解码出错几次、解出几帧（要是 0 错误）；`base shown`：它的 `shown`；
   - `half kbit` / `shown`（只对 L1T2）：同尺寸、一半帧率、码率等于 L1T2 第 0 层实际用的码率的一份单独编码（就是“低档编码”），它给弱观看端的画质，和 `base shown` 比。
   - 比较时看**同样的实际码率**：码控在两种结构下用掉的码率不一样（不受限时 L1T2 常多用码率），所以拿 L1T2 一行和 L1T1 里实际码率相近的一行比。
2. **第二个会话**：按 `--fps` 实时送 `--seconds` 秒，量编码器每帧的时间（交进去到出包）p50 / p95，先单独一个会话，再和第二个会话（同尺寸、一半帧率、一半码率，同一个实现）一起；打印第二个会话自己的时间，以及主编码有没有因此换了工作方式（macOS：切到流水线编码器）。这一部分对时间敏感，测的时候机器上不要有别的重负载。

```sh
# 合成的画面（打字、滚动），1080p60，480 帧，四个码率
rcsdk_encoder_layers --bitrates 450k,1.5m,2.5m,5m --csv layers.csv
# 只测某些编码器：--codecs h264,hevc,av1,vp8,vp9  --acceleration hardware|software  --backend NVIDIA（名字里含的）
# 真实屏幕：先在观看端录一段（rcsdk_cli --role view ... --record rec），再拿录下的码流当画面（按它的尺寸）
rcsdk_encoder_layers --input rec.h264 --bitrates 1.5m,5m
# 只测分层，或只测第二个会话
rcsdk_encoder_layers --layers-only ...      rcsdk_encoder_layers --sessions-only --seconds 10
```

- Windows：直接运行 `rcsdk_encoder_layers.exe`（NVENC 和 Media Foundation 各一遍，`--backend NVENC` 只测 NVENC；Media Foundation 的编码器打开后才知道能不能分层，做不到的只有 L1T1 一行；NVENC 的 HEVC、AV1 要驱动 570 起才分层，PC（RTX 4090，驱动 616.92）上验证过）。
- Android：用带工具的目录编，测试要开着（它用 SDK 的内部库 `rcsdk_internal`）：`cmake --preset android-arm64 -B build/android-tools -DRCSDK_BUILD_TOOLS=ON -DBUILD_TESTING=ON && cmake --build build/android-tools --target rcsdk_encoder_layers`，`adb push` 到 `/data/local/tmp` 后 `adb shell /data/local/tmp/rcsdk_encoder_layers --csv /data/local/tmp/layers.csv`，再 `adb pull` 结果。MediaCodec 的分层看编码器打开后输出格式里的 `ts-schema`。
- `--csv` 每行一个（编码器、画面、目标码率、层数），第二个会话的结果是 `content` 为 `sessions` 的行。

## 每个编码器、解码器每帧的时间

`rcsdk_codec_timing` 对一种编码格式和画面格式（`--codec h264|hevc|vp9|av1 --format 420|420-10|444|444-10`，默认 HEVC 4:4:4），按 `--fps`（默认 60）的节拍实时跑 `--seconds`（默认 10）秒，去掉开头 1 秒：

- 每个接这种格式的编码器：从交出画面到拿到包的时间 p50 / p95、均值，进程每帧的 CPU 时间（所有线程的用户加内核时间），实际码率。画面是白底上的彩色文字，上半屏每帧滚 3 像素；Windows 上先上传成编码器所在 GPU 上的 BGRA 纹理，和采集给的一样。
- 每个接这种格式的解码器：解同一路码流（排第一的硬件编码器编的，没有就用软件的），从交出包到画面在 GPU 上画完（等它的栅栏）的时间，即 `on_gpu_frame` 拿到的样子，和每帧的 CPU 时间。
- `--encoders` 或 `--decoders` 只测一边，`--backend NAME` 只测名字里有 NAME 的实现（例如 `--backend Intel`：oneVPL 和 Media Foundation 的 Intel 编码器），`--size`、`--bitrate`（`20m`、`500k`）。
- `--gpu NAME`（Windows）：Media Foundation 的硬件解码器在哪块 GPU 上都能解，默认在默认 GPU 上；给了就在名字里有 NAME 的那块上解（例如 `--gpu Intel`，和同一块核显上的 oneVPL 解码器比）。

```sh
# Windows：NVDEC、oneVPL（Intel 核显）和 CPU 上的软件解码器解 HEVC 4:4:4，1080p 和 4K
rcsdk_codec_timing --codec hevc --format 444 --decoders
rcsdk_codec_timing --codec hevc --format 444 --size 3840x2160 --bitrate 40m --decoders
# Windows：同一块 Intel 核显上 Media Foundation 和 oneVPL 的解码器解 4:2:0
rcsdk_codec_timing --codec hevc --format 420 --decoders --gpu Intel
# Windows：同一块 Intel 核显上 oneVPL 和 Media Foundation 的编码器
rcsdk_codec_timing --codec h264 --format 420 --encoders --backend Intel
```

这一部分对时间敏感，测的时候机器上不要有别的重负载。

## 转发器的负载

`rcsdk_forwarder_load` 量一台转发器（[指南 24](../docs/guide/24-forwarder.md)）带得动多少房间和观看端、代价多少（结果见 [PERFORMANCE](../docs/PERFORMANCE.md) 的“转发器的吞吐”）。

- **分享端**（每个房间一个）：和 SDK 的分享端一样建到转发器的那一跳（`ForwarderHello` publish），按码率和帧率发封装好的画面（平均大小 = 码率 ÷ 8 ÷ 帧率，普通帧 ±25%；关键帧是平均的 `--keyframe-factor` 倍，开始时、转发器要时、`--keyframe-interval-ms` 到时各一个），`--audio on` 时每秒 `--sound-packets` 个 `--sound-bytes` 字节的声音包（默认 100 个 120 字节：约 96 kbit/s 的 Opus 按 10 ms 一包，WASAPI 的周期；Mac 的 Core Audio 进程捕获是 5 ms 一包，ScreenCaptureKit 20 ms）；`--encodings 2` 时另有一档四分之一码率的低档，跟不上的观看端被转发器移到这一档。那一跳的码率和 SDK 的分享端默认一样是动态的（从 4 Mbit/s 起，最多发的 4 倍），画面大小跟着它量到的上行收（SDK 也这样限住编码）；`--sharer-rate fixed` 时都不跟。观看端的第一个管道数据报代表它端到端的握手：分享端这时才订阅它（流 0 的第 0 档和声音，和 SDK 一样在放进来之后），之后把它管道里的数据报原样送回。
- **观看端**：和 SDK 的观看端一样建那一跳（`ForwarderHello` subscribe），确认、报告、补要都是 SDK 自己的连接做的；但**不跑**管道里端到端的连接，也不开封、不解码：画面的封装里明文写着分享端交出它的时间和序号（转发器从不看里面），观看端读出来。所以转发器看到的和真的观看端一样，而一台机器能放几百个（真的观看端每个要解码）。每个观看端每 250 ms 经管道发一个数据报（像端到端连接的 ping），分享端送回：经转发器的控制和输入的往返。
- **量的**（预热 `--warmup` 秒之后的 `--seconds` 秒）：每个观看端收到的帧率；画面延迟（分享端把画面交给那一跳 → 观看端那一跳交出整帧，含两跳的节流和转发器；分享端和观看端在同一个进程里，同一个时钟）；没收到的画面（按序号）、那一跳的重传和校验修复、放弃的帧；声音的延迟和丢失；管道的往返；观看端那一跳收发的字节（转发器发给它们的，加上它们很少的报告）。`--pid` 给出转发器的进程号时，每秒读它的 CPU（100% = 一个核）、常驻内存和线程数，本进程的 CPU 另算——同一台机器上负载和转发器抢 CPU，看的是两个数各自多少。
- **慢的观看端**（`--slow N --slow-bps 2m [--slow-loss 千分比] [--slow-delay ms]`，只走 UDP）：第 0 个房间的前 N 个观看端经一个限速的代理（队列 150 ms）连转发器，单独统计；其余的观看端那一行看它们受不受影响。
- **入口**：`--entry udp|tcp|websocket|mix`（`mix` 轮流三种）给观看端，`--sharer-entry` 给分享端；TCP、WebSocket 入口的端口用 `--tcp-port`、`--websocket-port`。
- 输出：每秒一行（观看端平均 / 最低帧率、这一秒的延迟 p50 / p95、转发器 CPU、内存、线程、本进程 CPU、观看端那一跳的流量），结束时每组观看端一行汇总，`--per-viewer` 每个观看端一行（附转发器报告给分享端的那条路径：目标码率、丢包、积压、丢掉积压的次数），最后一行 `RESULT key=value ...` 给脚本用。
- **转发器在另一台机器上**（例如 Linux 服务器或虚拟机）：在那台机器上 `rcsdk_forwarder_load monitor --pid <转发器进程号> --interface eth0` 每秒打一行（Unix 时间、CPU、内存、线程、网卡收发的 Mbit/s 和包数、这一秒系统因 UDP 套接字缓冲满丢的数据报：`udp_rcvbuf_errors`、`udp_sndbuf_errors`），取负载那次运行打印的时间窗（`window: unix A .. B`）里的行。Linux 上的工具用带工具的目录编：`cmake --preset linux-forwarder -B build/fwload -DRCSDK_BUILD_TOOLS=ON -DBUILD_TESTING=OFF && cmake --build build/fwload --target rcs_forwarder rcsdk_forwarder_load`。

```sh
# 转发器（配置里 max_viewers_per_room 要够大，最多 256）
rcs_forwarder --generate-identity id.key
rcs_forwarder --config fwd.conf &            # listen_udp = 127.0.0.1:47995, listen_tcp ..., secret = 1:<64 位十六进制>
# 一个房间 50 个观看端，1080p60 约 8 Mbit/s，带声音，测 20 秒
rcsdk_forwarder_load run --forwarder 127.0.0.1:47995 --secret 1:<十六进制> --identity id.key \
    --rooms 1 --viewers 50 --fps 60 --bitrate 8m --seconds 20 --pid $!
# 10 个房间各 5 人，经 WebSocket 入口；一个慢的观看端（2 Mbit/s），分享端有低档
rcsdk_forwarder_load run ... --websocket-port 47996 --entry websocket --rooms 10 --viewers 5
rcsdk_forwarder_load run ... --viewers 10 --slow 1 --slow-bps 2m --encodings 2 --per-viewer
```

- 连接时每次最多 `--connect-parallel`（默认 8）个一起握手：转发器同时只收 32 个还没交令牌的端。
- 在 macOS 上分享端每帧的发送靠 `sleep_until`，几十个房间时各有一个线程；负载进程自己的 CPU 一行里看得到，它接近满时数字就不可信了。

## 发送端的阶段时间

用 CMake 选项 `-DRCSDK_TRACE=ON` 构建时，Windows 发送端在标准错误输出每个阶段的时间戳（默认不编译进去）。
