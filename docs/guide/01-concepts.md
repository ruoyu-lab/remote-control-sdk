# 01 概览与基本概念

本页讲清楚 SDK 的模型：角色、会话、状态、核心配置和扩展链、预设、运行中修改，最后给出能直接编译的最小分享端和观看端（C 和 C++）。各个平台怎样编进自己的程序见 03～06。

## SDK 做什么、宿主做什么

- **数据面归 SDK**：屏幕和声音的采集、编解码、加密传输（局域网、P2P 穿越 NAT、TURN 中继，或宿主自己的通道）、动态码率、丢包恢复、远程输入的执行。
- **控制面归宿主**：账号、身份和配对、邀请、审批、界面，以及 P2P / 中继时把 SDK 给的信令文本转给对方。SDK 不依赖任何服务器，也没有界面。
- **不悄悄降级**：硬件还是软件编解码、提供哪些编码格式、要 GPU 帧还是 CPU 帧，都由宿主配置。做不到时返回明确的结果码和原因（见 [18 错误处理](18-errors.md)），由宿主决定下一步，例如改用软件解码、从 P2P 改走中继。宿主明确要求的“退路”（`RCS_ACCELERATION_HARDWARE_FIRST`：硬件解不了的格式用软件）照常有效。
- **宿主**指用 SDK 编写的程序：独立的远程桌面和远程协助软件、嵌入看屏和控屏的协作 / 教育 / 客服 / 即时通讯应用、设备管理和云桌面、只在局域网里用的工具。SDK 对它们一视同仁。

## 角色和会话

- 一个**会话**（`rcs_session`）是一端的一次远程连接的全部：配置、回调、统计。两个角色用同一种句柄：
  - **分享端**（`RCS_ROLE_SHARE`）：监听（或经 P2P、中继、宿主通道等待），把画面、声音交给观看端，按授权执行对方的输入。默认同时服务最多 64 个观看端（`RCS_EXT_VIEWERS`），每个观看端在这次运行里有一个编号 `rcs_viewer_id`（1、2、3……，不重复使用；0 表示“不针对谁”）。
  - **观看端**（`RCS_ROLE_VIEW`）：连接分享端，收画面和声音，可以发输入、发麦克风。
- 用错角色的函数返回 `RCS_ERROR_UNSUPPORTED`，属于另一个角色的扩展是 `RCS_ERROR_INVALID_ARGUMENT`。
- 两端必须用同一个 SDK 版本（网络协议的版本在握手里，0.1.2-beta 是 v1），见 [21 版本、分发与许可](21-versions-distribution.md)。

## 生命周期和状态

```text
rcs_session_create ──▶ IDLE ──rcs_session_start──▶ STARTING ──▶ LISTENING（分享端）/ CONNECTING（观看端）
                                                                     │
                                                                     ▼
          STOPPED ◀──rcs_session_stop── RUNNING ◀── 握手、协商完成 ──┘
                                          │  ▲
              路径没了 / 连接断开（观看端）  ▼  │ 路径回来 / 重新连上
                                      RECONNECTING
          FAILED ◀── 本端的失败、或按设置不再继续时
```

- `rcs_session_start` 是异步的：进度通过 `on_event` 的 `RCS_EVENT_STATE` 报告（`rcs_state_event`：`value` 和 `result`）。做不到的配置在 `start` 里就被拒绝（返回值，加上 `rcs_session_get_failure` 的原因）。
- 两端都在握手和协商完成后进入 `RUNNING`，之后才有这个连接的编解码器、画面、声音、控制权事件。画面的开始看 `RCS_EVENT_STREAM`。
- 连接断开时分享端默认回到 `LISTENING` 等下一个观看端，观看端默认进入 `RECONNECTING` 重连 30 秒（`RCS_EXT_RECONNECT`，见 [09](09-transports.md#断线常驻监听与自动重连)）。换网络时 UDP、P2P、中继和 HOST 上的连接默认留着、换路径（[09](09-transports.md#网络变化连接迁移)）：路径没了时观看端也报告 `RECONNECTING`，路径回来后回到 `RUNNING`，还是同一个连接，没有新的 `PEER`、`STREAM` 事件。
- `rcs_session_stop`（或 `rcs_session_stop_with_reason`）停止这次运行，返回后这次运行的回调不会再来；停止或失败的会话可以再次 `start`。`rcs_session_destroy` 停止并释放。不能在会话自己的回调里调用 start / stop / destroy（`RCS_ERROR_CALLBACK_THREAD`），见 [02](02-threading.md)。

## 配置：核心配置加扩展链

`rcs_session_config`（用 `RCS_SESSION_CONFIG_INIT` 初始化）只有四样宿主要填的东西，其余都有默认值：

| 字段 | 内容 | 默认 |
| --- | --- | --- |
| `role` | `RCS_ROLE_SHARE` 或 `RCS_ROLE_VIEW` | 观看 |
| `preset` | 宿主没放进扩展链的扩展用哪套值，见下文“预设” | `RCS_PRESET_BALANCED` |
| `connect` | `transport`：`RCS_TRANSPORT_UDP`（默认）、`TCP`、`P2P`、`RELAY`、`HOST`，两端一致。UDP 和 TCP 时 `address`（数字的 IPv4 或 IPv6）和 `port`：分享端绑定，观看端连接；P2P 和中继时 `port` 是本地 UDP 端口（0：系统选），`address` 不用；HOST 两个都不用。见 [09](09-transports.md) | UDP，`127.0.0.1`，47990 |
| `identity` | `secret`：`RCS_SECRET_KEY`（`key`，32 字节）、`PASSWORD`、`PIN`（都放在 `password` 里）或 `NONE`；`pin_attempts`。见 [10](10-security.md) | `KEY`；`key` 必须由宿主填，全零是 `INVALID_ARGUMENT`；`pin_attempts` 5 |

`version` 由 `RCS_SESSION_CONFIG_INIT` 填成头文件的 `RCSDK_VERSION`，`struct_size` 填成本版本的大小；不用 `*_INIT` 初始化的结构体会被拒绝（`INVALID_ARGUMENT`）。`extensions` 是可选的扩展链：

- 每个扩展是一个以 `rcs_extension { type, struct_size, next }` 开头的结构体，用自己的 `*_INIT` 宏初始化（宏填好 `type`、`struct_size` 和 `BALANCED` 的值），用 `next` 串起来，顺序不限，每种最多一次（模块按 id 算）。
- 不放进链的扩展取预设的值；**放进链的扩展完全是宿主的**：预设不往里面填字段。要“某个预设、只改一个字段”，先用 `rcs_preset_defaults(preset, &ext.header)` 拿到那个预设的整份扩展再改。
- 不属于这个角色的扩展是 `INVALID_ARGUMENT`（不会被忽略）；本系统用不上的字段（别的平台的）由 `start` 以 `UNSUPPORTED` 拒绝；本版本没有的类型、枚举值、`struct_size` 不对，都是 `INVALID_ARGUMENT`。
- `rcs_session_create` 复制核心配置、整条扩展链和回调表，之后宿主可以释放它们（例外：回调的 `user_data` 和模块设置里指向的东西归宿主，要活得比会话久）。

扩展分三层，层次只是头文件的划分，代码都在核心库里，不用另外链接：

| 层 | 头文件（C++） | 内容 |
| --- | --- | --- |
| 入门 | `rcsdk/rcsdk.h` 的核心配置和回调（`rcsdk/rcsdk.hpp`） | 角色、预设、怎么连、怎么证明身份；开始和停止、事件、取画面 |
| 功能 | `rcsdk/rcsdk.h` 的功能扩展；`rcsdk/module.h` 和各个模块的头文件 | 每个功能一个扩展，只放宿主的决定 |
| 调优 | `rcsdk/tuning.h`（`tuning.hpp`） | 引擎的参数：码率规则、发送节奏、队列、声音缓冲、网络超时（[13](13-tuning.md)） |
| 替换 | `rcsdk/host_codec.h`、`rate_control.h`、`host_transport.h`、`gpu.h`、`agent.h`（各有 `.hpp`） | 宿主接管内部的一环（[17](17-replacing.md)、[16](16-unattended.md)）；回调在扩展里，带自己的 `user_data` |
| 平台 | `rcsdk/apple.h`、`rcsdk/android.h`（各有 `.hpp`） | 系统自己的类型：`CMSampleBuffer`、Java VM、`AHardwareBuffer`；别的系统上是 `UNSUPPORTED` |

功能层的扩展（都在 `rcsdk/rcsdk.h`，运行中都能用 `rcs_session_update` 替换）：

| 扩展 | 角色 | 内容（详见） |
| --- | --- | --- |
| `rcs_share_video_config`（`RCS_EXT_SHARE_VIDEO`） | 分享 | 发送的尺寸、帧率、码率、码率模式和下限；是否照办观看端的要求及上限（[07](07-sharing.md)） |
| `rcs_encoder_config`（`RCS_EXT_ENCODER`） | 分享 | 提供的编码格式和画面格式、矩阵和范围、硬件或软件、后端（[07](07-sharing.md)） |
| `rcs_capture_config`（`RCS_EXT_CAPTURE`） | 分享 | 共享什么：显示器、区域、窗口、应用、宿主的画面或码流；隐藏本应用的窗口；跟随安全桌面（[07](07-sharing.md)） |
| `rcs_streams_config`（`RCS_EXT_STREAMS`） | 分享 | 更多显示器，每块一路，最多 4 路（[07](07-sharing.md#多显示器)） |
| `rcs_control_config`（`RCS_EXT_CONTROL`） | 分享 | 是否允许控制、谁执行输入（[11](11-input.md)） |
| `rcs_viewers_config`（`RCS_EXT_VIEWERS`） | 分享 | 同时几个观看端、码率跟谁、准入超时（[07](07-sharing.md#一对多)） |
| `rcs_encodings_config`（`RCS_EXT_ENCODINGS`） | 分享 | 跟不上或解不了主编码的观看端拿的低档编码：自动、只一档或固定几档，各档的帧率、像素率和编码格式（[07](07-sharing.md#弱的观看端多一档编码rcs_ext_encodings)） |
| `rcs_view_request_config`（`RCS_EXT_VIEW_REQUEST`） | 观看 | 向分享端要的画面、声音、指针、要哪几路（[08](08-viewing.md)） |
| `rcs_decoder_config`（`RCS_EXT_DECODER`） | 观看 | 接受的编码格式和画面格式、硬件 / 软件 / 硬件优先、后端（[08](08-viewing.md)） |
| `rcs_send_audio_config`（`RCS_EXT_SEND_AUDIO`） | 两端 | 本端发出的声音：本机播放的、麦克风或宿主推的（[12](12-audio.md)） |
| `rcs_receive_audio_config`（`RCS_EXT_RECEIVE_AUDIO`） | 两端 | 本端收到的声音：由 SDK 播放还是交给宿主（[12](12-audio.md)） |
| `rcs_background_config`（`RCS_EXT_BACKGROUND`） | 两端 | 应用进后台时自动暂停什么（iOS；[05](05-quickstart-ios.md)） |
| `rcs_ice_servers_config`（`RCS_EXT_ICE_SERVERS`） | 两端 | P2P 的 STUN、中继的 TURN 服务器和凭据（[09](09-transports.md)） |
| `rcs_reconnect_config`（`RCS_EXT_RECONNECT`） | 两端 | 连接断了这次运行怎么办（[09](09-transports.md#断线常驻监听与自动重连)） |
| `rcs_device_identity_config`（`RCS_EXT_DEVICE_IDENTITY`） | 两端 | 本端的设备密钥、信任哪些对端（[10](10-security.md)） |
| `rcs_log_config`（`RCS_EXT_LOG`） | 两端 | 这个会话自己的日志级别（[14](14-events-stats-logging.md#日志)） |
| `rcs_module_config`（`RCS_EXT_MODULE`） | 两端 | 一个模块和它的设置，一条链里可以有多个（[15](15-modules.md)） |

每个扩展的默认值见 [reference/defaults.md](reference/defaults.md)。

## 预设

预设（`rcs_preset`）决定宿主没放进链的扩展用哪套值：`BALANCED`（默认：H.264 硬件、1920×1080、60 FPS、20 Mbit/s 动态码率）、`LOW_LATENCY`、`LOW_BANDWIDTH`、`QUALITY`。只有 `SHARE_VIDEO`、`ENCODER`、`SEND_AUDIO`、`VIEW_REQUEST` 和三个调优扩展有预设的值；共享什么、给不给控制、身份、服务器、观看端的解码器这些宿主的决定不随预设变。`rcs_session_get_settings` 读出会话现在实际用的任意一个扩展，`rcs_session_set_preset` 运行中换预设。详见 [13](13-tuning.md#预设)。

## 运行中修改

`rcs_session_update(session, changes)` 传入和配置相同的扩展（可以串成链），整条链要么全部生效，要么都不生效（原因见 `rcs_get_last_call_error`）：

- 空闲、已停止或失败时：可以改本角色的任何扩展，下次 `start` 用。
- 运行中：功能层和调优层的扩展都能改，大多立即生效（画面或声音按需要带关键帧重新开始，连接不断）。`RCS_EXT_DEVICE_IDENTITY`、`RCS_EXT_ICE_SERVERS` 的服务器和凭据（TURN 证书指纹和名字也算）、`RCS_EXT_NETWORK_TUNING` 的网卡和超时从下一个连接起生效（分享端再次监听、观看端重连时）。`RCS_EXT_ICE_SERVERS` 的 `turn_transport` 和替换层的全部扩展在一次运行中固定，运行中改是 `INVALID_STATE`。
- 每个扩展整体替换：没写的字段就是结构体里的值。所以先读出现在用的（`rcs_session_get_settings`，C++ `session.settings<T>()`）再改。
- 放进 `rcs_session_update` 的扩展从此是宿主的，之后换预设不再改它。
- 核心配置不经 `rcs_session_update`：换身份用 `rcs_session_set_identity`（下一个握手起），换预设用 `rcs_session_set_preset`；角色和 `connect` 在会话的一生中不变。

## 编解码协商

每一端提供 `codecs[]` 里本机用所选加速方式和后端能实现的格式（分享端 `rcs_encoder_config`，观看端 `rcs_decoder_config`），默认只有 H.264；一个都没有时 `start` 返回 `UNSUPPORTED`。分享端用**自己列表中第一个、观看端也提供了的**格式，观看端的顺序不影响结果，但观看端可以用 `rcs_view_request_config.codec` 要求两端都有的另一个。画面格式（4:4:4、10 位、HDR10）和编码格式一起协商（[13](13-tuning.md#画面格式)）。没有交集时两端都以 `UNSUPPORTED` 失败，原因写明双方各提供了什么，`rcs_peer_info`（`RCS_EVENT_PEER`、`rcs_session_get_peer_info`）给出对端的列表。SDK 从不添加宿主没列出的格式：列出第二个格式，就是宿主接受这个替代。

## 最小的完整程序

下面两个程序只用公开头文件，在 macOS 和 Windows 上编译运行（iOS 和 Android 的程序由系统的应用框架启动，分享端的画面由宿主推，见 05、06；其他系统没有 SDK 自己的采集和编解码，`start` 以 `UNSUPPORTED` 说明，见 [19](19-platforms.md)）。分享端第一次运行时生成 32 字节的连接密钥写进文件，把文件拷到观看端（真实的宿主经自己的通道交换密钥，或改用口令、PIN、设备密钥，见 [10](10-security.md)）。

`common.h`：

```c
/* common.h：两个示例共用的小工具。 */
#include <rcsdk/rcsdk.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h> /* 链接 -framework CoreFoundation */
/* macOS：主线程的 run loop 要转着，系统框架会往上面投递工作。 */
static void wait_ms(unsigned ms) { CFRunLoopRunInMode(kCFRunLoopDefaultMode, ms / 1000.0, false); }
#elif defined(_WIN32)
#include <windows.h>
static void wait_ms(unsigned ms) { Sleep(ms); }
#else
#include <time.h>
static void wait_ms(unsigned ms) {
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}
#endif

static volatile sig_atomic_t stopping = 0;
static void on_interrupt(int sig) {
    (void)sig;
    stopping = 1;
}

/* 32 字节的连接密钥：文件在就读，不在就生成并写进去。 */
static int load_or_make_key(const char* path, uint8_t key[32], int may_create) {
    FILE* f = fopen(path, "rb");
    if (f) {
        const size_t n = fread(key, 1, 32, f);
        fclose(f);
        return n == 32 ? 0 : -1;
    }
    if (!may_create || rcs_generate_connection_key(key, 32) != RCS_OK || !(f = fopen(path, "wb"))) {
        return -1;
    }
    fwrite(key, 1, 32, f);
    fclose(f);
    return 0;
}

static void print_failure(rcs_session session, const char* what) {
    rcs_result result = RCS_OK;
    char message[512] = "";
    rcs_session_get_failure(session, &result, message, sizeof message);
    fprintf(stderr, "%s: %s: %s\n", what, rcs_result_message(result), message);
}
```

`sharer.c`：

```c
/* sharer.c：分享本机的主显示器（画面和声音），观看端走了继续等下一个；Ctrl+C 结束。
 *   sharer <本机地址> <密钥文件>     密钥文件不存在时生成一个，交给观看端 */
#include "common.h"

static void on_event(const rcs_event* e, void* user_data) {
    (void)user_data; /* SDK 的线程；事件之间不重叠 */
    switch (e->type) {
    case RCS_EVENT_STATE:
        printf("状态 %u（%s）\n", e->state.value, rcs_result_message(e->state.result));
        break;
    case RCS_EVENT_VIEWER:
        printf("观看端 %u %s\n", e->viewer_id, e->viewer.joined ? "加入" : "离开");
        break;
    case RCS_EVENT_STREAM:
        printf("画面 %ux%u %u FPS\n", e->stream.info.width, e->stream.info.height, e->stream.info.frames_per_second);
        break;
    default:
        break;
    }
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "用法：sharer <本机地址> <密钥文件>\n");
        return 2;
    }
    if (rcs_version_number() != RCSDK_VERSION) { /* 头文件和库必须是同一个版本 */
        fprintf(stderr, "库是 %s，头文件不是这个版本\n", rcs_version());
        return 1;
    }
    if (rcs_screen_capture_permission() == RCS_PERMISSION_REQUIRED) { /* macOS：屏幕录制 */
        rcs_request_screen_capture_permission(); /* 主线程；用户授权后重新启动程序 */
        fprintf(stderr, "请在系统设置里允许屏幕录制，然后重新运行\n");
        return 1;
    }

    rcs_session_config config = RCS_SESSION_CONFIG_INIT; /* 其余都是默认值（BALANCED） */
    config.role = RCS_ROLE_SHARE;
    snprintf(config.connect.address, sizeof config.connect.address, "%s", argv[1]); /* 端口默认 47990 */
    if (load_or_make_key(argv[2], config.identity.key, 1) != 0) {
        fprintf(stderr, "读不了也写不了 %s\n", argv[2]);
        return 1;
    }

    /* 功能扩展（可选）：有输入权限时允许对方控制键鼠。 */
    rcs_control_config control = RCS_CONTROL_CONFIG_INIT;
    control.allowed = rcs_input_permission() == RCS_PERMISSION_GRANTED;
    config.extensions = &control.header; /* 更多扩展用 header.next 串起来 */

    rcs_session_callbacks callbacks = RCS_SESSION_CALLBACKS_INIT;
    callbacks.on_event = on_event;

    rcs_session session = NULL;
    if (rcs_session_create(&config, &callbacks, &session) != RCS_OK) {
        char reason[512];
        rcs_get_last_call_error(reason, sizeof reason); /* 例如哪个扩展的哪个字段越界 */
        fprintf(stderr, "创建失败：%s\n", reason);
        return 1;
    }
    if (rcs_session_start(session) != RCS_OK) { /* 做不到的配置在这里就被拒绝，不会悄悄降级 */
        print_failure(session, "启动失败");
        rcs_session_destroy(session);
        return 1;
    }
    signal(SIGINT, on_interrupt);
    while (!stopping) {
        rcs_state state = RCS_STATE_IDLE;
        rcs_session_get_state(session, &state);
        if (state == RCS_STATE_FAILED) {
            print_failure(session, "运行失败");
            break;
        }
        wait_ms(200);
    }
    rcs_session_destroy(session); /* 停止（告诉对端）、等所有回调结束、释放 */
    return 0;
}
```

`viewer.c`：

```c
/* viewer.c：连上分享端，每秒打印收到的帧数；Ctrl+C 结束。
 *   viewer <分享端地址> <密钥文件> */
#include "common.h"

#include <rcsdk/gpu.h>

static void on_event(const rcs_event* e, void* user_data) {
    (void)user_data;
    if (e->type == RCS_EVENT_STATE) {
        printf("状态 %u（%s）\n", e->state.value, rcs_result_message(e->state.result));
    } else if (e->type == RCS_EVENT_CODEC) {
        printf("解码器 %s\n", e->codec.name);
    } else if (e->type == RCS_EVENT_CONTROL) {
        printf("控制权 %s\n", e->control.granted ? "已授予" : "没有");
    }
}

/* 解码线程：每一帧都是原生 GPU 表面。要留到回调之后就 rcs_gpu_frame_retain，用完 release。 */
static void on_gpu_frame(rcs_gpu_frame frame, void* user_data) {
    (void)user_data;
    rcs_gpu_frame_info info = RCS_GPU_FRAME_INFO_INIT;
    if (rcs_gpu_frame_get_info(frame, &info) == RCS_OK) {
        /* info.resource：macOS / iOS 是 CVPixelBufferRef，Windows 是 ID3D11Texture2D*（info.device 是它的
         * ID3D11Device*），Android 是 AHardwareBuffer*；info.format 是布局（默认 NV12），info.color 是颜色。 */
    }
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "用法：viewer <分享端地址> <密钥文件>\n");
        return 2;
    }
    rcs_session_config config = RCS_SESSION_CONFIG_INIT; /* role 默认就是 RCS_ROLE_VIEW */
    snprintf(config.connect.address, sizeof config.connect.address, "%s", argv[1]);
    if (load_or_make_key(argv[2], config.identity.key, 0) != 0) {
        fprintf(stderr, "读不了 %s（从分享端拷过来）\n", argv[2]);
        return 1;
    }

    /* 功能扩展（可选）：HEVC 优先，也接受 H.264；默认只有 H.264。 */
    rcs_decoder_config decoder = RCS_DECODER_CONFIG_INIT;
    decoder.codecs[0] = RCS_CODEC_HEVC;
    decoder.codecs[1] = RCS_CODEC_H264;
    config.extensions = &decoder.header;

    rcs_session_callbacks callbacks = RCS_SESSION_CALLBACKS_INIT;
    callbacks.on_event = on_event;
    callbacks.on_gpu_frame = on_gpu_frame; /* 或者 on_frame：CPU 上的 BGRA8，二选一 */

    rcs_session session = NULL;
    if (rcs_session_create(&config, &callbacks, &session) != RCS_OK) {
        char reason[512];
        rcs_get_last_call_error(reason, sizeof reason);
        fprintf(stderr, "创建失败：%s\n", reason);
        return 1;
    }
    if (rcs_session_start(session) != RCS_OK) { /* 例如没有 GPU 的虚拟机：UNSUPPORTED 和原因 */
        print_failure(session, "启动失败");
        rcs_session_destroy(session);
        return 1;
    }
    signal(SIGINT, on_interrupt);
    uint64_t last = 0;
    while (!stopping) {
        wait_ms(1000);
        rcs_state state = RCS_STATE_IDLE;
        rcs_session_get_state(session, &state);
        if (state == RCS_STATE_FAILED) {
            print_failure(session, "运行失败");
            break;
        }
        rcs_video_receive_stats stats = RCS_VIDEO_RECEIVE_STATS_INIT;
        if (rcs_session_get_stats(session, RCS_STATS_VIDEO_RECEIVE, &stats) == RCS_OK) {
            printf("%llu 帧/秒\n", (unsigned long long)(stats.delivered_frames - last));
            last = stats.delivered_frames;
        }
    }
    rcs_session_destroy(session);
    return 0;
}
```

C++ 包装（`rcsdk/rcsdk.hpp`，只有头文件）的同样两个程序：

```cpp
// sharer.cpp：和 sharer.c 一样，用 C++ 包装（rcsdk/rcsdk.hpp）。
//   sharer <本机地址> <密钥文件>
#include <rcsdk/rcsdk.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <thread>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>   // 链接 -framework CoreFoundation
#endif

// 等一会儿；macOS 上同时转主线程的 run loop（系统框架会往上面投递工作）。
static void waitFor(std::chrono::milliseconds duration) {
#if defined(__APPLE__)
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, std::chrono::duration<double>(duration).count(), false);
#else
    std::this_thread::sleep_for(duration);
#endif
}

namespace {
std::atomic<bool> stopping{false};

rcsdk::ConnectionKey loadOrMakeKey(const char* path) {
    rcsdk::ConnectionKey key{};
    if (std::ifstream in(path, std::ios::binary); in.read(reinterpret_cast<char*>(key.data()), key.size())) {
        return key;
    }
    key = rcsdk::generateConnectionKey();
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(key.data()), key.size());
    return key;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "用法：sharer <本机地址> <密钥文件>\n";
        return 2;
    }
    try {
        auto config = rcsdk::SessionConfig(RCS_ROLE_SHARE)   // 预设默认 BALANCED
                          .setAddress(argv[1], 47990)
                          .setKey(loadOrMakeKey(argv[2]))
                          .setControl(rcsdk::inputPermission() == RCS_PERMISSION_GRANTED);
        rcsdk::Callbacks callbacks;
        callbacks.onEvent = [](const rcs_event& e) {   // SDK 的线程；不能抛异常
            if (e.type == RCS_EVENT_STATE) {
                std::printf("状态 %u（%s）\n", e.state.value, rcs_result_message(e.state.result));
            } else if (e.type == RCS_EVENT_VIEWER) {
                std::printf("观看端 %u %s\n", e.viewer_id, e.viewer.joined ? "加入" : "离开");
            }
        };
        rcsdk::Session session(config, std::move(callbacks));   // 创建失败抛 rcsdk::Error
        if (session.start() != RCS_OK) {
            std::cerr << "启动失败：" << session.failure().message << "\n";
            return 1;
        }
        std::signal(SIGINT, [](int) { stopping = true; });
        while (!stopping && session.state() != RCS_STATE_FAILED) {
            waitFor(std::chrono::milliseconds(200));
        }
        if (const auto failure = session.failure()) {
            std::cerr << rcs_result_message(failure.result) << "：" << failure.message << "\n";
        }
    } catch (const rcsdk::Error& error) {   // 例如扩展的字段越界：原因来自 rcs_get_last_call_error
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;   // Session 析构时停止会话并等所有回调结束
}
```

```cpp
// viewer.cpp：和 viewer.c 一样，用 C++ 包装。
//   viewer <分享端地址> <密钥文件>
#include <rcsdk/rcsdk.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>   // 链接 -framework CoreFoundation
#endif

// 等一会儿；macOS 上同时转主线程的 run loop（系统框架会往上面投递工作）。
static void waitFor(std::chrono::milliseconds duration) {
#if defined(__APPLE__)
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, std::chrono::duration<double>(duration).count(), false);
#else
    std::this_thread::sleep_for(duration);
#endif
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "用法：viewer <分享端地址> <密钥文件>\n";
        return 2;
    }
    rcsdk::ConnectionKey key{};
    if (!std::ifstream(argv[2], std::ios::binary).read(reinterpret_cast<char*>(key.data()), key.size())) {
        std::cerr << "读不了 " << argv[2] << "\n";
        return 1;
    }
    static std::atomic<bool> stopping{false};
    try {
        auto config = rcsdk::SessionConfig(RCS_ROLE_VIEW)
                          .setAddress(argv[1], 47990)
                          .setKey(key)
                          .setCodecs({RCS_CODEC_HEVC, RCS_CODEC_H264});   // 两端都有 HEVC 时用 HEVC
        rcsdk::Callbacks callbacks;
        callbacks.onEvent = [](const rcs_event& e) {
            if (e.type == RCS_EVENT_STATE) {
                std::cout << "状态 " << e.state.value << "（" << rcs_result_message(e.state.result) << "）\n";
            }
        };
        callbacks.onGpuFrame = [](const rcsdk::GpuFrame& frame) {
            const rcs_gpu_frame_info info = frame.info();   // 复制 GpuFrame 就留住了这一帧（引用计数）
            (void)info;   // 交给渲染线程：info.resource、info.format、info.color
        };
        rcsdk::Session session(config, std::move(callbacks));
        if (session.start() != RCS_OK) {
            std::cerr << "启动失败：" << session.failure().message << "\n";
            return 1;
        }
        std::signal(SIGINT, [](int) { stopping = true; });
        uint64_t last = 0;
        while (!stopping && session.state() != RCS_STATE_FAILED) {
            waitFor(std::chrono::seconds(1));
            const auto stats = session.stats<rcs_video_receive_stats>();
            std::cout << stats.delivered_frames - last << " 帧/秒\n";
            last = stats.delivered_frames;
        }
    } catch (const rcsdk::Error& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}
```

C++ 包装的约定：名词返回所指的东西（`capabilities()`、`session.streamInfo()`、`config.encoder()` 返回整个 C 结构体），`set*` 修改设置，动词执行动作；动作返回 `rcs_result`，构造、设置和产生新东西的函数失败时抛 `rcsdk::Error`，会话的状态和信息查询是 `noexcept`。`rcsdk::SessionConfig(role, preset)` 只把设置过的扩展挂进链，第一次碰到一个扩展时从配置的预设开始；设置函数没有覆盖的字段用 `config.shareVideo()`、`config.encoder()`、`config.capture()` 等直接改整个结构体，调优和替换层的扩展用 `config.extension<T>()` 取、`config.use(ext)` 放。

编译（以 CMake 为例；各平台的细节见 03～06）：

```cmake
cmake_minimum_required(VERSION 3.25)
project(minimal C CXX)
find_package(RemoteControlSDK 0.1 REQUIRED)   # 或 add_subdirectory(remote-control-sdk)
add_executable(sharer sharer.c)
add_executable(viewer viewer.c)
target_link_libraries(sharer PRIVATE RemoteControl::Core)
target_link_libraries(viewer PRIVATE RemoteControl::Core)
if(APPLE)
    target_link_libraries(sharer PRIVATE "-framework CoreFoundation")
    target_link_libraries(viewer PRIVATE "-framework CoreFoundation")
endif()
```

```sh
./sharer 192.168.1.10 key.bin          # 被控的机器
./viewer 192.168.1.10 key.bin          # 另一台机器（key.bin 从分享端拷过来）
```

没有第二个自己的程序时，可以用示例 `rcsdk_cli` 当另一端（[22](22-examples-tools.md)）。

## 术语

| 术语 | 含义 |
| --- | --- |
| 宿主 | 用 SDK 编写的程序 |
| 分享端 / 观看端 | 会话的两种角色（`RCS_ROLE_SHARE` / `RCS_ROLE_VIEW`） |
| 运行 | 从 `rcs_session_start` 到停止或失败；一次运行里可以有多个连接（观看端来去、重连） |
| 主观看端 | 一对多时尺寸、格式等要求由它决定的那个观看端（最早加入的，或宿主指定的）；`RCS_BITRATE_FROM_MAIN` 时码率也跟它 |
| 档（encoding） | 同一路画面的一份编码：主编码（0 档）给所有人，低档（1、2 档）给跟不上或解不了主编码的观看端（`RCS_EXT_ENCODINGS`） |
| 路（stream） | 一路画面；多显示器时每块显示器一路，编号 0～3 |
| 助手（agent） | 分享端和这台机器有关的那一半（采集、编码、声音采集、输入执行），默认在会话的进程里，也可以在另一个进程（[16](16-unattended.md)） |
| 看护者（supervisor） | Windows 服务里保持一个助手进程在用户会话里运行的组件（[16](16-unattended.md)） |
| 模块 | 挂在核心之外的功能，宿主注册、在会话里启用（剪贴板、文件传输……，[15](15-modules.md)） |
| 组件 | 不属于会话的官方库（看护者） |
| 预设 | 宿主没放进链的扩展用的一套值 |
