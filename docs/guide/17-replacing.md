# 17 替换内部的一环：宿主的编解码器、编码包、码率决定

宿主可以接管 SDK 内部的一环，SDK 照旧做其余的。每一种有自己的头文件和扩展；头文件都包含 `rcsdk.h`，代码在核心库里，不用另外链接。回调放在它的扩展里，带它自己的 `user_data`。这些扩展没有预设，**在一次运行中固定**：运行中用 `rcs_session_update` 放进它们返回 `INVALID_STATE`（C++ 的 `Session::update(config)` 会跳过它们）。宿主实现回调的对象要活得比会话久。

| 头文件（C++） | 扩展和函数 | 用途 | 本文 |
| --- | --- | --- | --- |
| `rcsdk/host_codec.h`（`host_codec.hpp`） | `RCS_EXT_HOST_ENCODER`、`RCS_EXT_HOST_DECODER`；画面来源 `RCS_CAPTURE_ENCODED` 的 `rcs_session_push_encoded_video`、`rcs_session_video_ready`；`RCS_EXT_ENCODED_OUTPUT` | 宿主自己编码、解码或直接给码流；两端拿到编码包（录制、转发） | 下文 |
| `rcsdk/rate_control.h`（`rate_control.hpp`） | `RCS_EXT_RATE_DECIDER` | 宿主自己决定码率 | 下文 |
| `rcsdk/host_transport.h`（`host_transport.hpp`） | `RCS_EXT_HOST_TRANSPORT`、`rcs_session_transport_receive`、`rcs_session_transport_closed` | 连接走宿主自己的通道 | [09](09-transports.md#宿主自己的传输rcs_transport_host) |
| `rcsdk/gpu.h`（`gpu.hpp`） | `rcs_gpu_frame_*`、`RCS_EXT_GPU_DEVICE`、`rcs_session_push_gpu_frame` | GPU 帧；在宿主的 D3D11 设备上解码；宿主推 GPU 画面 | [08](08-viewing.md)、[04](04-quickstart-windows.md)、[07](07-sharing.md#宿主自己的画面和声音) |
| `rcsdk/agent.h`（`agent.hpp`） | `RCS_EXT_AGENT_LINK`、`rcs_agent_*`、`rcs_helper_*` | 采集、编码和输入放在另一个进程里的助手 | [16](16-unattended.md) |

C++ 里宿主实现一个小接口类（`rcsdk::HostEncoder`、`HostDecoder`、`EncodedOutput`、`RateDecider`、`HostTransport`、`AgentGate`），用 `rcsdk::hostEncoder()`、`hostDecoder()`、`encodedOutput()`、`rateDecider()`、`hostTransport()`、`agentLink()`、`gpuDevice()` 组装成扩展，`config.use(...)` 放进配置。

## 宿主自己的编解码器

默认用系统的编解码器（VideoToolbox、Media Foundation / NVENC、MediaCodec）。宿主可以接管一端的编码或解码，SDK 照旧负责协商、传输、加密、动态码率、关键帧请求和丢包恢复、统计和事件。适合直接调 NVENC / AMF / Quick Sync 的 SDK、用 x264 / SVT-AV1 这样的软件编码器（VP8、VP9、AV1 和 Linux 上的 H.264 SDK 已经自带软件实现，见 [13](13-tuning.md#软件编解码)）、解码进自己的渲染管线（D3D12、Vulkan、Metal）、已经有码流的游戏引擎和云渲染，以及两端都由宿主实现的自定义格式。

- **编码格式**：标准格式（H.264、HEVC、VP8、VP9、AV1）的码流照 `rcs_encoded_packet` 的约定（H.264 / HEVC：带 4 字节大端长度前缀的 NAL 单元，`config` 是参数集；VP8 / VP9：编码器写出的一帧（VP9 可以是带索引的超帧），`config` 是 `vpcC` 记录，宿主可以不给、SDK 按流补上；AV1：低开销格式的 OBU，不含时间单元分隔符，`config` 是序列头），所以一端用宿主的、另一端用 SDK 自带的也行。`RCS_CODEC_HOST_FIRST`（16）到 `RCS_CODEC_HOST_LAST`（31）是宿主自定义的格式，含义由两端的宿主约定，SDK 当作不透明的字节传，两端都要有宿主的编解码器。协商规则不变；宿主负责的格式算本机能用，不受加速方式和后端的筛选。列了自定义格式却没有宿主负责时，创建会话返回 `INVALID_ARGUMENT`。
- **宿主的编码器**（分享端，`rcs_host_encoder_config`，C++ 实现 `rcsdk::HostEncoder`）：`codecs[]` 是它负责的格式，`picture_formats` 是它能编的画面格式（SDR），`cpu_images` 为 1 时总是交 CPU 上的 BGRA8，`name` 出现在事件和统计里。SDK 照常采集，在每路、每档编码（`RCS_EXT_ENCODINGS` 的低档也各开一个，`stream` 和 `encoding` 说明是哪个）的编码线程上依次调用 `open`（开始和格式、尺寸变化时；`rcs_host_encoder_settings` 带着路号、档号、格式、颜色、尺寸、帧率、码率、`speed`、要做的时域层数 `temporal_layers`（这一档的 `scalability`，不超过宿主在 `rcs_host_encoder_config.temporal_layers` 里说它最多做的，默认 1）、Windows 上的 D3D11 设备和交包的 `output`）、`encode`（每一帧：`rcs_host_encoder_frame` 的 `gpu_frame` 或 `cpu_frame`、`keyframe`、`sequence`）、`set_bitrate(stream, encoding, ...)`（动态码率改目标时）、`close(stream, encoding)`。编好的包用 `rcs_encoder_output_push(settings.output, packet)` 交回，在 `encode` 里或之后从别的线程都行（异步编码器）；包里的 `sequence` 填对应画面的编号，编号 N 的包表示 N 和更早的画面都处理完了。SDK 最多让 3 帧在途，所以编码器不能等后面的画面才出包（不能有前瞻和 B 帧）。交回的包先检查（格式、尺寸、NAL / OBU 边界、关键帧带 `config`；路号和档号按打开时的算），不合格返回 `INVALID_ARGUMENT` 并说明原因；`temporal_id` 要小于打开时的层数；标准格式的 `temporal_id` 和 `discardable` 码流说得出的以码流为准（和码流不一致时记一次日志），说不出的（没有 SVC 前缀单元的 H.264 的层号、AV1 的“可丢弃”、VP8 和 VP9 的层号）和宿主自定义格式的照包里填的；分层的编码器要守“一帧只参考自己这层和更低层、第 0 层以外的帧不参考最近一个第 0 层之前的高层帧”，观看端才能少拿层、再从第 0 层的帧起多拿；`open` 或 `encode` 返回错误时本次运行以宿主返回的结果失败。
- **宿主直接给码流**（分享端，画面来源 `RCS_CAPTURE_ENCODED`）：SDK 不采集也不编码，提供 `rcs_encoder_config` 列出的全部格式。宿主看 `RCS_EVENT_KEYFRAME`（流开始、有人要、到了 `keyframe_interval_ms`）编关键帧，看 `RCS_EVENT_RATE` 调码率；每帧先问 `rcs_session_video_ready`（`QUEUE_FULL` 时路径上还压着约两帧或码率预算用完了，这一帧不要编），再用 `rcs_session_push_encoded_video` 交码流，格式和尺寸要是当前流的（`rcs_stream_info`），每一路流的第一个包必须是关键帧。不能和另一个进程里的助手、也不能和 `RCS_EXT_HOST_ENCODER` 同时用。
- **宿主的解码器**（观看端，`rcs_host_decoder_config`，C++ 实现 `rcsdk::HostDecoder`）：在解码线程上，`open` 在流的第一个关键帧到达时（带尺寸），格式、尺寸变化时先 `close` 再 `open`；`decode` 按顺序收每个包（排队、过时丢弃、丢包后从关键帧继续都在它之前做完）；丢过包时先 `reset`。`decode` 返回错误时 SDK 关掉它、重新 `open`、从下一个关键帧继续（`RCS_EVENT_RECOVERY`，`RCS_RECOVERY_DECODER`）。解出的画面由宿主自己显示，SDK 不调用 `on_frame` / `on_gpu_frame`。
- **助手在另一个进程**：编码在助手里，所以宿主的编码器交给助手：`rcs_agent_config.extensions` 里放 `RCS_EXT_HOST_ENCODER`（C++ `AgentConfig::setHostEncoder`），会话一侧再给是 `INVALID_ARGUMENT`。
- **事件和统计**里的编码器、解码器名字是宿主给的 `name`，`backend` 是 `RCS_BACKEND_HOST`。宿主暂时拿不到 HDR 画面（`UNSUPPORTED`）。

```cpp
#include <rcsdk/host_codec.hpp>

class MyEncoder final : public rcsdk::HostEncoder {
    rcs_result open(const rcs_host_encoder_settings& s) override { output_ = s.output; /* 打开 NVENC…… */ return RCS_OK; }
    rcs_result encode(const rcs_host_encoder_frame& f) override {
        // f.gpu_frame（rcs_gpu_frame_get_info）或 f.cpu_frame；f.keyframe
        // 编好后（这里或输出线程上）：rcsdk::pushEncoded(output_, packet)，packet.sequence = f.sequence
        return RCS_OK;
    }
    rcs_result setBitrate(uint32_t stream, uint32_t encoding, uint32_t bitsPerSecond) override { /* 改码率 */ return RCS_OK; }
    void close(uint32_t stream, uint32_t encoding) override { /* 关掉 */ }
    rcs_encoder_output output_ = nullptr;
};
MyEncoder encoder;   // 活得比会话久
auto config = rcsdk::SessionConfig(RCS_ROLE_SHARE).setAddress("0.0.0.0", 47990).setKey(key)
                  .setCodecs({RCS_CODEC_HEVC, RCS_CODEC_H264})
                  .use(rcsdk::hostEncoder(encoder, {RCS_CODEC_HEVC}, "my NVENC"));   // HEVC 用自己的，H.264 仍用系统的
```

## 编码包交给宿主：录制、转发、审计

两端都能拿到编码包（`RCS_EXT_ENCODED_OUTPUT`，`rcs_encoded_output_config`），不用解码再编码：录下远程协助、远程教学的过程，给客服、运维留审计记录，把码流转给宿主自己的媒体服务器。两个回调至少设一个：`on_video(packet, user_data)` 拿视频包（`rcs_encoded_packet`），`on_audio(packet, user_data)` 拿声音包（`rcs_encoded_audio`）。包只在调用期间有效，要留就复制；回调要立即返回，写磁盘交给自己的线程。两个回调来自不同的线程，可能同时进行，各自按顺序来。

- **分享端**：`on_video` 拿到发出之前的每一个视频包（编码器的线程上，一份编码给它那一档的所有观看端）；默认只有主编码的，`encodings` 位掩码（`1u << encoding`）可以要低档的（`RCS_EXT_ENCODINGS`），包里的 `encoding` 说明是哪一档；`on_audio` 拿到发出之前的每一个声音包（每个收声音的观看端有自己的一串包，`viewer_id` 说明是谁的）。
- **观看端**：`on_video` 拿到收到的每一个视频包，在解码之前、按解码的顺序、在解码线程上；分享端把这一端换到另一档编码时，包从那一档的关键帧接着来，`encoding` 变了，编码格式和尺寸可能也变了（录文件时从这里另起或重新开始）；`on_audio` 拿到收到的每一个声音包，在解码之前、在声音线程上。
- **包里有什么**：视频包有编码格式、画面格式和颜色、尺寸、是否关键帧、序号，标准格式的每个包都带着当前的参数集（`config`）；声音包有 `codec`、48 kHz 立体声、`frames`、`sequence`（空缺就是丢了）。Opus 是一个 Opus 包，AAC 是不带 ADTS 头的原始单元（`config` 是 AudioSpecificConfig），PCM 是 16 位小端交错。
- **时间线**：两种包的 `timestamp_ns` 都是分享端的采集时间（分享端的单调时钟），宿主直接拿来做文件里画面和声音的时间戳，两者就对得上。
- **从关键帧开始**：开始录制时调 `rcs_session_request_keyframe`，第一个关键帧之前的包不写。
- **丢了画面**：补不回来的帧（`RCS_RECOVERY_LOSS`）、接收队列丢掉的积压（`RCS_RECOVERY_BACKLOG`，例如 `on_video` 太慢）之后，下一个交出的包带 `after_gap = 1`：它是关键帧，或者是参考帧失效后只参考丢失之前那些帧的一帧，文件从这里接着写就能解码。写磁盘偶尔慢的录制服务器可以放宽 `queue_frames`、把 `max_queue_age_ms` 设成 0（[13](13-tuning.md)）。
- **只录不看**（观看端 `decode = 0`）：不开视频解码器，声音也不解码、不播放；`on_frame`、`on_gpu_frame`、`on_audio` 必须为空，必须有 `on_video`，不能同时放 `RCS_EXT_HOST_DECODER`。报给分享端的是宿主能存什么（`rcs_decoder_config` 列出的编码格式和画面格式原样报），不是本机能解什么。iOS 上想在后台继续录，把 `RCS_EXT_BACKGROUND` 的 `pause` 设成 0。
- **录制要不要告诉对方是宿主的事**：SDK 不在画面上加标记，也不通知另一端；需要时宿主用数据通道或自己的模块告诉对方“正在录制”。
- SDK 不写 MP4 / MKV：容器格式（分段、索引、断电保护）由宿主用自己的库决定。示例 `rcsdk_cli --role view ... --record rec [--record-only]`（代码在 `examples/common/recorder.hpp`）写 Annex B / OBU 和 Ogg Opus / ADTS / WAV，可以参考写法。

```cpp
#include <rcsdk/host_codec.hpp>

class Recorder final : public rcsdk::EncodedOutput {
    void video(const rcs_encoded_packet& packet) override {
        if (waiting && !packet.keyframe) {   // 文件从关键帧开始；after_gap 的包接着写就行
            return;
        }
        waiting = false;
        writer.push(packet);   // 复制需要的内容交给写文件的线程，不要在这里等磁盘
    }
    void audio(const rcs_encoded_audio& packet) override { writer.push(packet); }
    bool waiting = true;
};
Recorder recorder;   // 活得比会话久
auto config = rcsdk::SessionConfig(RCS_ROLE_VIEW).setAddress("192.168.1.10", 47990).setKey(key)
                  .use(rcsdk::encodedOutput(recorder, false));   // 只录不看
rcsdk::Session session(config);
session.start();
// 到了 RCS_STATE_RUNNING：
session.requestKeyframe();
```

## 宿主决定码率

动态码率的测量总是 SDK 做的；决定可以交给宿主（`rcsdk/rate_control.h`，C++ `rate_control.hpp`）。`RCS_EXT_RATE_DECIDER` 的 `on_rate_report(report, decision, user_data)` 在每份报告到达时调用（视频在流动时约每 50 ms 一次）：`rcs_rate_report` 里是这份报告本身（窗口、应收和实收的包、字节、单向时延）、SDK 算出的排队延迟、平滑后的丢包和到达速率、往返时间、这个连接换过几次路径（`path_changes`：变了就是换到了一条新路径，排队延迟的基准从那里重新开始，丢包和时延会跳一下，不是拥塞）、编码器最近的实际输出、本机还没发出的视频、当前目标和上下限，以及是哪个观看端的连接（`viewer_id`）；`decision->bitrate_bps` 里是规则的建议。改它就是宿主自己的决定（例如通话时封顶，或者自己的拥塞控制算法），不动就是只看；SDK 把决定限制在上下限里，变化 5% 以上时发 `RCS_EVENT_RATE`。回调在连接的网络线程上，一次一份，要立即返回。固定码率和 TCP 上不调用。发送节奏仍按规则的阈值切换。网络统计的 `rate_reports`、`rate_overrides` 是决定过的报告数和宿主改过的数。

```cpp
#include <rcsdk/rate_control.hpp>

// 宿主自己在通话时，视频最多 2 Mbit/s；其余时候照规则
class CallCeiling final : public rcsdk::RateDecider {
public:
    std::atomic<bool> inCall{false};
    void decide(const rcs_rate_report&, rcs_rate_decision& decision) override {
        if (inCall) {
            decision.bitrate_bps = std::min(decision.bitrate_bps, 2'000'000u);
        }
    }
};
CallCeiling ceiling;   // 活得比会话久
config.use(rcsdk::rateDecider(ceiling));
```
