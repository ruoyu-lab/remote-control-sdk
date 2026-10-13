# 05 快速开始：iOS / iPadOS

SDK 可以编进 iOS 和 iPadOS 应用（iOS 17 起）：当观看端看、控 Mac 和 Windows；当分享端分享应用自己的画面，或经广播扩展分享整个屏幕。iOS 不允许一个应用采集整个屏幕或操作别的应用，所以分享端的画面由宿主推（`RCS_CAPTURE_HOST`），对方的输入交给宿主执行（`RCS_INPUT_HOST`），整个屏幕只能看。真机还没有验证过（见 [ROADMAP](../ROADMAP.md)）；模拟器里的测试全部通过。

## 引入 SDK

- **XCFramework**：`scripts/build-xcframework.sh [输出目录]`（默认 `build/xcframework`）编出 `RemoteControl.xcframework`：真机 arm64、模拟器 arm64 + x86_64 的静态库，核心、编进 SDK 的编解码库和全部官方模块合成一个 `libRemoteControl.a`，带 C / C++ 头文件和 `module.modulemap`。拖进 Xcode 工程，再链接系统框架 Foundation、UIKit、CoreVideo、CoreMedia、VideoToolbox、Security、Network、AudioToolbox、AVFoundation、ImageIO、CoreGraphics 和 `libc++`。
  - Objective-C / C++：`#include <rcsdk/rcsdk.h>`（C++ 包装 `rcsdk.hpp` 也在里面）。
  - Swift：`import RemoteControlSDK`。`*_INIT` 宏导入不了 Swift，用同名的初始化函数（`rcs_session_config_init()` 等），见 [02 的 Swift 一节](02-threading.md#swiftiosmacos)。
- 也可以用预设 `ios-device` / `ios-simulator` 编静态库，或把 SDK 当 CMake 子项目用 `-DCMAKE_SYSTEM_NAME=iOS` 编进宿主的工程（iOS 上默认编静态库）。
- `Info.plist`：
  - `NSMicrophoneUsageDescription`：用麦克风时。
  - `NSLocalNetworkUsageDescription`：iOS 14 起，应用第一次连局域网地址时系统弹窗问用户，没允许之前连接失败。系统没有查询这个授权的接口；连不上局域网地址时提示用户去“设置 › 隐私与安全性 › 本地网络”。模拟器不弹这个窗。局域网广播和组播（例如设备发现）要向苹果申请组播权利，SDK 自己不用。
- **模拟器没有硬件编解码器**：默认的 `RCS_ACCELERATION_HARDWARE` 在模拟器上 `start` 时以原因失败，观看端用 `RCS_ACCELERATION_HARDWARE_FIRST` 或 `SOFTWARE`（模拟器有 H.264 的软件编码器和 H.264、HEVC 的软件解码器）。真机有硬件 H.264 / HEVC 编解码，A17 Pro、M3 起还有 AV1 解码。硬件编码要 iOS 17.4 才能明确要求“必须硬件”或“必须软件”，17.0～17.3 上是 `UNSUPPORTED`。

## 观看端

- 配置和桌面一样（[01](01-concepts.md#最小的完整程序)、[08](08-viewing.md)）。GPU 帧是 `CVPixelBufferRef`，用 `CVMetalTextureCache` 零复制画出（和 macOS 相同，见 [03](03-quickstart-macos.md#观看端用-metal-显示画面)）。
- **进后台**：应用进了后台不能用 GPU，系统还会让编解码器失效。SDK 默认跟着应用的状态走：没有一个场景在前台时暂停画面，回到活动状态时恢复（`RCS_EVENT_PAUSE` 的 `by_background`）；暂停什么由 `RCS_EXT_BACKGROUND` 的 `pause` 决定（默认 `RCS_PAUSE_VIDEO`；只录不看的观看端想在后台继续录，设成 0）。iPad 上一个应用有几个窗口时，某个窗口单独到了后台由宿主对它的会话调用 `rcs_session_pause`。没有后台音频权限的应用进了后台，系统很快会挂起它，连接随之断开，回来后自动重连（观看端默认 30 秒内）。
- **触屏变成输入**是宿主的事（[11](11-input.md#触屏和移动端)）：点按发 `BUTTON`，触控板模式发 `MOVE` 或 `MOVE_RELATIVE`，两指拖动发 `SCROLL`，外接键盘的 `UIKey.keyCode` 就是 HID usage、直接发 `KEY`，软键盘打的字发 `TEXT`，Apple Pencil 发 `PEN`。

## 声音会话

iOS 上 SDK 的声音（所有会话的播放和麦克风）都跑在应用的一个 AVAudioEngine 上，AVAudioSession 整个应用共用。默认（`RCS_AUDIO_SESSION_SDK`）由 SDK 设：只播放时类别 `playback`；有麦克风时 `playAndRecord`，开回声消除时模式 `voiceChat`；第一个开始时激活、最后一个停了才取消激活；来电、Siri、别的应用的打断让声音暂停并报告原因（`RCS_EVENT_AUDIO`），结束后自动恢复。宿主自己管声音会话时调用一次：

```c
rcs_audio_session_config audio = RCS_AUDIO_SESSION_CONFIG_INIT;
audio.manager = RCS_AUDIO_SESSION_HOST;   /* SDK 不再碰声音会话；设得不能播放或录音时声音以原因暂停 */
rcs_set_audio_session_config(&audio);
```

`SDK` 模式的选项：`mix_with_others = 1` 和别的应用的声音一起放（默认让它们停下）；`default_to_speaker`（默认 1）录麦克风时从扬声器放，0 时 iPhone 从听筒放；`bluetooth_microphone`（默认 1）允许蓝牙耳机的麦克风。麦克风见 [12](12-audio.md)；回声消除消掉的是经同一个音频引擎播出的声音，也就是 SDK 播放的对方的声音。

## 分享端：应用自己的画面

用 ReplayKit 的应用内录制（`RPScreenRecorder.startCaptureWithHandler`，不需要扩展；开始前系统会问用户）。画面和声音以 `CMSampleBuffer` 到手，原样交给 `rcs_session_push_sample_buffer`（`rcsdk/apple.h`）：SDK 在 GPU 上按方向转正、按原来的形状放进流的尺寸（其余是黑的）；应用的声音和麦克风（`microphoneEnabled` 时）各自转成 48 kHz 立体声，混成一路发出（见下面的“两种声音”）。流的尺寸由宿主定（按屏幕的形状，转屏时用 `rcs_session_update` 换），`rcs_stream_info` 报告画面在哪里（`content_*`）和本来多大（`display_width` / `display_height`）。

```objc
#import <ReplayKit/ReplayKit.h>
#import <UIKit/UIKit.h>
#include <rcsdk/apple.h>
#include <rcsdk/rcsdk.h>
#include <string.h>

// 分享这个 App 自己的画面和声音；对方的输入交给 App（on_input）。
static rcs_session start_app_sharing(const uint8_t key[32], rcs_session_callbacks* callbacks) {
    rcs_capture_config capture = RCS_CAPTURE_CONFIG_INIT;
    capture.source = RCS_CAPTURE_HOST;   // 画面由宿主推
    rcs_send_audio_config audio = RCS_SEND_AUDIO_CONFIG_INIT;
    audio.source = RCS_AUDIO_HOST;       // 声音由宿主推：App 的声音和麦克风，SDK 混成一路
    rcs_control_config control = RCS_CONTROL_CONFIG_INIT;
    control.allowed = 1;
    control.target = RCS_INPUT_HOST;     // iOS 不许操作别的 App：输入交给 on_input
    rcs_share_video_config video = RCS_SHARE_VIDEO_CONFIG_INIT;
    const CGSize pixels = UIScreen.mainScreen.nativeBounds.size;   // 竖屏的像素尺寸；流按屏幕的形状定
    video.width = ((uint32_t)pixels.width / 2) & ~1u;
    video.height = ((uint32_t)pixels.height / 2) & ~1u;
    capture.header.next = &audio.header;
    audio.header.next = &control.header;
    control.header.next = &video.header;

    rcs_session_config config = RCS_SESSION_CONFIG_INIT;
    config.role = RCS_ROLE_SHARE;
    strcpy(config.connect.address, "0.0.0.0");
    memcpy(config.identity.key, key, 32);
    config.extensions = &capture.header;
    rcs_session session = NULL;
    if (rcs_session_create(&config, callbacks, &session) != RCS_OK || rcs_session_start(session) != RCS_OK) {
        return NULL;   // 原因：rcs_get_last_call_error / rcs_session_get_failure
    }
    RPScreenRecorder.sharedRecorder.microphoneEnabled = YES;   // 也录麦克风（系统会问用户）
    [RPScreenRecorder.sharedRecorder
        startCaptureWithHandler:^(CMSampleBufferRef buffer, RPSampleBufferType type, NSError* error) {
            if (error) {
                return;
            }
            rcs_sample_buffer_info info = RCS_SAMPLE_BUFFER_INFO_INIT;   // orientation 0：读 ReplayKit 的附件
            info.type = (rcs_sample_buffer_type)type;   // 和 RPSampleBufferType 同值：画面、App 的声音、麦克风
            rcs_session_push_sample_buffer(session, buffer, &info);   // 还没有观看端时是 INVALID_STATE，丢掉这一帧
        }
        completionHandler:^(NSError* error) {
            (void)error;   // 用户拒绝录制时 error 不为空
        }];
    return session;
}
```

- `callbacks->on_input` 收到对方的输入（坐标是画面上的 0..65535，见 [11](11-input.md)），宿主在自己的界面里执行，例如变成自己窗口的触摸事件、文字交给有焦点的输入框。示例：`examples/qml-demo/app_input.cpp`。
- 也可以不经 sample buffer，直接推 `CVPixelBuffer`：`rcs_session_push_gpu_frame`（`rcsdk/gpu.h`，BGRA8、NV12、P010、NV24、P410），这时尺寸必须等于流的尺寸、方向要宿主自己转正。示例：`examples/qml-demo/app_capture_ios.mm`。
- **模拟器里 ReplayKit 能开始录制，但不出画面**：这部分只能在真机上试。
- iOS 不能录别的应用的声音：分享端发 ReplayKit 给的应用声音和麦克风（混成一路），或 SDK 自己录的麦克风（`RCS_AUDIO_MICROPHONE`）。

## 分享端：整个屏幕（广播上传扩展）

用户在系统的面板上开始直播后，系统在**单独的进程**里运行宿主的广播上传扩展（Broadcast Upload Extension），把整个屏幕（别的应用在前台也一样）、设备放的声音和麦克风（用户在面板上打开麦克风时）以 sample buffer 交给它。宿主应用在后台会被挂起，所以分享会话放在扩展里：

```objc
#import <ReplayKit/ReplayKit.h>
#include <rcsdk/apple.h>
#include <rcsdk/rcsdk.h>
#include <string.h>

// 广播上传扩展：系统在单独的进程里运行它，把整个屏幕、设备的声音和麦克风交过来。分享会话就放在这里。
@interface SampleHandler : RPBroadcastSampleHandler
@end

@implementation SampleHandler {
    rcs_session _session;
}

- (void)broadcastStartedWithSetupInfo:(NSDictionary<NSString*, NSObject*>*)setupInfo {
    // 地址、端口、密钥从 App 那里来：App Group 或两边共用的钥匙串访问组（宿主自己的事）。
    uint8_t key[32] = {0};
    /* loadSettings(key); */
    rcs_capture_config capture = RCS_CAPTURE_CONFIG_INIT;
    capture.source = RCS_CAPTURE_HOST;
    rcs_send_audio_config audio = RCS_SEND_AUDIO_CONFIG_INIT;
    audio.source = RCS_AUDIO_HOST;   // 设备的声音和麦克风（RPSampleBufferTypeAudioApp、AudioMic），SDK 混成一路
    capture.header.next = &audio.header;
    rcs_session_config config = RCS_SESSION_CONFIG_INIT;
    config.role = RCS_ROLE_SHARE;
    strcpy(config.connect.address, "0.0.0.0");
    memcpy(config.identity.key, key, 32);
    config.extensions = &capture.header;
    if (rcs_session_create(&config, NULL, &_session) != RCS_OK || rcs_session_start(_session) != RCS_OK) {
        char reason[256];
        rcs_get_last_call_error(reason, sizeof reason);
        [self finishBroadcastWithError:[NSError errorWithDomain:@"rcsdk" code:1 userInfo:@{
            NSLocalizedDescriptionKey : [NSString stringWithUTF8String:reason]}]];
    }
}

- (void)processSampleBuffer:(CMSampleBufferRef)sampleBuffer withType:(RPSampleBufferType)sampleBufferType {
    rcs_sample_buffer_info info = RCS_SAMPLE_BUFFER_INFO_INIT;   // 方向从 ReplayKit 的附件读
    info.type = (rcs_sample_buffer_type)sampleBufferType;          // 和 RPSampleBufferType 同值
    rcs_session_push_sample_buffer(_session, sampleBuffer, &info);
}

- (void)broadcastPaused {
    rcs_session_pause(_session, RCS_PAUSE_VIDEO | RCS_PAUSE_AUDIO);   // 观看端收到 by_peer
}

- (void)broadcastResumed {
    rcs_session_resume(_session, RCS_PAUSE_VIDEO | RCS_PAUSE_AUDIO);
}

- (void)broadcastFinished {
    rcs_session_destroy(_session);
    _session = NULL;
}
@end
```

要注意的：

- 扩展的内存上限约 50 MB：SDK 的画面一直在 GPU 上；会话本身约 20 MB（1080p，Mac 上量的），真机上要量自己的。
- 扩展里没有 `UIApplication`：SDK 在扩展里不观察前后台；保持唤醒模块报告不可用。
- 设置（地址、端口、密钥）怎么交给扩展是宿主的事：通常用 App Group；免费的个人开发团队不能用 App Group，可以用两边共用的钥匙串访问组（示例的做法）。
- 流的尺寸按屏幕的形状定（例子里用的是默认的 1920×1080，竖屏的画面会带黑边），转屏时 `rcs_session_update` 换成新的形状。示例在第一帧到达时按画面的形状启动会话。
- 打开系统的面板用 `RPSystemBroadcastPickerView`（`preferredExtension` 是自己的扩展）；用户也可以在控制中心长按“屏幕录制”选这个应用。
- 模拟器里广播开不起来，只能在真机上试。
- 示例：`examples/qml-demo/broadcast/`（`SampleHandler.mm`、设置经钥匙串）和应用里的“共享整个屏幕”。

### 两种声音

ReplayKit 把设备和应用放的声音（`RPSampleBufferTypeAudioApp`）和麦克风（`RPSampleBufferTypeAudioMic`）分开交过来，格式、每块的长度和节奏、时钟都不一样（常见的是前者 44.1 kHz 大端 16 位立体声、每块 1024 帧，后者 48 kHz 16 位单声道）。两种原样推给 `rcs_session_push_sample_buffer`（`RCS_SAMPLE_BUFFER_AUDIO`、`RCS_SAMPLE_BUFFER_MICROPHONE`），SDK 各自转换，再混成分享端的一路声音：

- **默认两种都发**：在手机上分享屏幕的人通常一边演示一边说话。只要其中一种时，把另一种的音量设成 0（它的缓冲照收、不发），或者干脆不推它：

  ```c
  rcs_send_audio_config audio = RCS_SEND_AUDIO_CONFIG_INIT;
  audio.source = RCS_AUDIO_HOST;
  audio.sample_buffer_audio_percent = 100;      /* 设备和应用的声音，0..400 */
  audio.sample_buffer_microphone_percent = 0;   /* 不要麦克风；150 是大一半 */
  ```

  C++：`config.setSampleBufferVolumes(100, 0)`。运行中用 `rcs_session_update` 改，立即生效，声音不重启（例如宿主界面上的“静音麦克风”）。两种相加后限制在满幅以内。
- **按到达混**：两种都在来时，先到的等另一种（最多大约它的一块，约 20 ms），所以哪种都不断；时间戳不比。
- **一种停了**：麦克风被关掉，或者应用都不出声（ReplayKit 这时可能就不再给这一种）。另一种最多再等“停了的那种最近两块之间最长的间隔加 20 ms”（几十毫秒），之后照常发；停了的那种再来就接着混。
- **延迟不会越积越长**：两种的时钟稍有不同，或者丢了一块，一种就会一直比另一种快；每秒看一次它一直多出来多少，到 10 ms 就去掉这么多（2 ms 的交叉淡化，日志里一条 `audio` 的 info）。
- 真机上 ReplayKit 实际给的格式和节奏（尤其应用不出声时还给不给缓冲）还没有验证，见 [ROADMAP](../ROADMAP.md)。

## 模块在 iOS 上

- `rcsdk.clipboard` 读写系统的剪贴板（UIPasteboard）：读别的应用复制的内容时，系统可能弹“允许粘贴”（iOS 16 起，除非用户在设置里允许），所以剪贴板默认关、由宿主打开；只看有没有新内容不会弹。
- `rcsdk.power` 让屏幕不熄（只接受 `DISPLAY`；`SYSTEM` 是 `UNSUPPORTED`），看着的 iPad 不自动锁屏。
- `rcsdk.text-caret`、`rcsdk.thumbnail` 只对能采集屏幕的分享端有意义，iOS 上报告不可用；`rcsdk.file-transfer` 照常可用。
