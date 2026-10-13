# iOS / iPadOS：平台说明

SDK 在 iOS 和 iPadOS 上做什么、怎样做、为什么这样做。怎样在自己的应用里用，见开发者指南的 [iOS / iPadOS 快速开始](guide/05-quickstart-ios.md)；接口以头文件为准（`include/rcsdk/rcsdk.h`、`rcsdk/apple.h`、`rcsdk/gpu.h`）。已实现观看端、宿主推画面的分享端、麦克风、模块、XCFramework 和经广播扩展分享整个屏幕（ReplayKit 的应用声音和麦克风混成一路）；iPad 真机还没有验证，验证到哪一步见 [ROADMAP](ROADMAP.md#当前状态)。

最低 iOS 17（和 macOS 14 同一代），iPadOS 同。`RCS_PLATFORM_IOS` 也指 iPadOS。

## 1. 能做什么

- **观看和控制 Mac、Windows**：远程桌面和远程协助的手机端、平板端，聊天里看对方的屏幕，设备管理的移动端。和桌面一样，填角色、地址、密钥就能看；画面以 GPU 帧（`CVPixelBuffer`）或 CPU 帧交出，声音默认由 SDK 播放。
- **分享应用自己的画面**：画面和声音由宿主推（ReplayKit 的应用内录制、自己渲染的内容；应用的声音或麦克风）。
- **分享整个屏幕**：经 ReplayKit 的广播上传扩展（第 9 节），只能看。
- **不能**：被别的设备控制（iOS 不许一个应用操作别的应用：`rcs_capabilities.input_types` 为 0，对方的输入只能交给宿主在自己的应用里执行，`RCS_INPUT_HOST`）；采集别的应用的画面和声音；tvOS、visionOS 不在计划里。

## 2. 目录和构建

- `src/platform/apple/`：和 macOS 共用的：VideoToolbox 编解码和编解码器列表、探测，像素缓冲和帧访问，推来的 `CVPixelBuffer` 和 sample buffer，AAC，Network.framework 的 TLS。
- `src/platform/ios/`：`system.mm`（能力、权限；采集和输入相关的返回 `UNSUPPORTED` 和原因）、`audio.mm`（声音设备、麦克风权限）、`audio_engine.mm`（整个应用的音频引擎和声音会话的协调者）、`app_state.mm`（前后台）。
- 预设 `ios-simulator`（arm64 模拟器，`ctest --preset ios-simulator` 在已启动的模拟器里跑测试）和 `ios-device`（只编译）。iOS 上默认编静态库；`scripts/build-xcframework.sh` 把核心、编解码库和官方模块合成 `RemoteControl.xcframework`（真机 arm64，模拟器 arm64 + x86_64），带 `module.modulemap`，Swift 可以 `import RemoteControlSDK`（Swift 导入不了 `*_INIT` 宏，见[指南 02](guide/02-threading.md#swiftiosmacos)）。
- 测试只编能在 iOS 上跑的（采集、注入、看护者、无人值守不编）；示例只编 `rcsdk_cli` 和 QML 示例；`tools/` 不编。
- 模块：`file_transfer` 可移植，照编；`clipboard`、`power` 有 iOS 的实现；`text_caret`、`thumbnail` 只在能采集屏幕的分享端有意义，在 iOS 上报告不可用。

## 3. 解码和画面

- 解码器和 macOS 共用：`VTIsHardwareDecodeSupported` 决定列不列硬件解码器；10 位、4:4:4 按内置的探测码流在运行时试出（AV1 硬件解码要 A17 Pro、M3 起的芯片），列表只报真的能开的。
- 画面：`RCS_GPU_CVPIXELBUFFER`，IOSurface 支撑、和 Metal 兼容，和 macOS 相同（软件解码器也输出这种缓冲，`software_decoding_gpu_frames` 为 1）。CPU 帧转成 BGRA。
- **模拟器**没有硬件编解码器：默认的硬件解码在模拟器上 `start` 时以原因失败，要用 `RCS_ACCELERATION_HARDWARE_FIRST` 或 `SOFTWARE`。模拟器的软件解码器解 H.264 4:2:0，HEVC 4:2:0、4:2:0 10 位、4:4:4、4:4:4 10 位；有 H.264 的软件编码器。

## 4. 暂停和进后台

- **暂停**（`rcs_session_pause` / `resume`，所有系统、两种角色）：宿主用来处理 iPad 上的多个窗口（一个到了后台，另一个还在前台）、被别的界面完全挡住、自己的流程。观看端暂停画面时不再解码、解码器关掉，分享端照“这个观看端不要这几路”处理；恢复时从关键帧重新开始。分享端暂停画面时不采集、不编码、不发，每个观看端经 `RCS_EVENT_PAUSE` 的 `by_peer` 得知。连接、输入、数据、模块都不受影响。
- **进后台自动暂停**（`RCS_EXT_BACKGROUND`，两种角色）：进了后台的应用不能用 GPU，VideoToolbox 的解码和编码会话会失效；如果照常运行，解码器出错恢复后在后台又会失败，运行就结束了。所以 SDK 默认跟着应用的状态走：进后台（`UIApplicationDidEnterBackgroundNotification`）按 `pause` 暂停（默认只停画面），回到活动（`UIApplicationDidBecomeActiveNotification`：可以用 GPU 了）时恢复；在后台时重复收到“活动”不算变化。只录不看（`decode = 0`）想在后台继续录的宿主把 `pause` 设成 0。不是应用的进程（模拟器里的命令行程序、App 扩展）从不算在后台。
- 宿主的暂停、进后台的暂停、对方的暂停各记各的：宿主的 `resume` 只取消宿主暂停的那些，回到前台只取消自动暂停的那些。
- 连接在后台被系统收走时，自动重连（观看端默认 30 秒）接上；超过了就照常以原因结束。

## 5. 声音

**声音会话（AVAudioSession）归谁**：它是整个应用共用的一个设置，SDK 不该在宿主不知道的时候改掉它；但“填几个字段就能用”又要求默认就能出声。一个应用里可能同时有几个会话，各自设置会互相覆盖，所以这是应用级的函数（`rcs_set_audio_session_config` / `rcs_get_audio_session_config`），不是会话的扩展；别的系统上是 `UNSUPPORTED`。

- `RCS_AUDIO_SESSION_SDK`（默认）：一个应用级的协调者统一管。每个要用声音的部分（某个会话的播放、麦克风）向它登记，它按所有登记合起来设类别（只播放：`playback`；有麦克风时 `playAndRecord`，开回声消除时模式 `voiceChat`）、`mix_with_others`、`default_to_speaker`（录音时从扬声器放）、`bluetooth_microphone`（允许蓝牙耳机的麦克风），第一个登记时激活，最后一个走了才取消激活。系统的打断、媒体服务重置也由它统一收。
- `RCS_AUDIO_SESSION_HOST`：SDK 一点不碰，宿主自己设。设得不对（例如要录音而类别是 `playback`）时，声音以原因暂停（`RCS_EVENT_AUDIO`），不悄悄改。
- 运行中可以改：换成 `HOST` 时协调者不再改任何东西，换回 `SDK` 时按现在的登记设一次。

**音频引擎**：整个应用一个 AVAudioEngine。每个播放是混音器上的一个 `AVAudioSourceNode`，麦克风是输入节点上的一个 tap（转成 48 kHz 立体声），回声消除是输入节点的语音处理。iOS 的语音处理只消掉经同一个 I/O 单元播出的声音，所以播放和麦克风必须在同一个引擎上，SDK 播放的对方的声音才会被消掉。引擎在自己的线程上按“现在有哪些部分”重新搭（部分来去、设置改变、系统打断、线路或格式改变、媒体服务重置），搭不起来时每 2 秒再试，把每个部分的状态（在跑、原因、语音处理）报告给它。

- 播放默认每周期 256 帧（48 kHz 下 5.3 ms），用 `preferredIOBufferDuration` 要；宿主明确给了周期而系统给的不一样时，那个播放以 `UNSUPPORTED` 和原因报告。
- 输出跟着系统的线路走（扬声器、耳机、AirPods）。iOS 不许应用自己选输出设备，只能用系统的界面（`AVRoutePickerView`），这是宿主的界面。
- 被打断（来电、Siri、别的应用抢占）时声音暂停并报告原因，打断结束后自动恢复。声音出问题只暂停声音，不结束运行。
- 麦克风的设备列表来自 `AVAudioSession.availableInputs`（内置麦克风、耳机、蓝牙、USB）；权限用 `AVAudioApplication`，任何线程都能调用。宿主的 `Info.plist` 要有 `NSMicrophoneUsageDescription`。
- iOS 不许采集别的应用的声音：分享端的 `RCS_AUDIO_SYSTEM` 是 `UNSUPPORTED`，应用的声音由宿主推（`RCS_AUDIO_HOST`）。

## 6. 网络

- **IPv6 地址**：iOS 拿不到地址的状态标志（失效、临时），只能把 `getifaddrs` 给的全局和唯一本地地址都列为候选；多出来的临时地址由连通检查自己挑。
- **本地网络授权**（iOS 14 起）：应用第一次连局域网地址时系统弹窗，没允许之前连接失败。系统没有查询这个授权的接口，SDK 不做探测。宿主的 `Info.plist` 要有 `NSLocalNetworkUsageDescription`。模拟器不弹这个窗；真机上失败时报告什么还没有验证。
- 广播和组播：SDK 自己不用。QML 示例的局域网发现靠 UDP 广播，iOS 上要向苹果申请组播权限，所以示例在 iOS 上手输地址。
- 其余和 macOS 相同：TLS 用 Network.framework；Wi-Fi 和蜂窝之间切换走路径断开和自动重连。

## 7. 输入（观看端发出的）

现有的输入类型都够用，触屏怎么变成输入是宿主的事（[指南 11](guide/11-input.md#触屏和移动端)）：点按发 `BUTTON`；触控板模式发 `MOVE_RELATIVE`，直接点哪里发 `MOVE`；两指拖动发 `SCROLL`；捏合缩放只放大本地画面；外接键盘的 `UIKey.keyCode` 就是 HID 用法码，直接发 `KEY`；软键盘打的字发 `TEXT`；Apple Pencil 发 `PEN`（压力、倾斜、方位），触摸也可以原样发 `TOUCH`。坐标换算用 `rcsdk/view_geometry.hpp`。

## 8. 模块

- `rcsdk.power`：关掉应用的空闲计时器（`idleTimerDisabled`，在主线程上设），所以只接受 `RCS_KEEP_AWAKE_DISPLAY`，`SYSTEM` 是 `UNSUPPORTED`（应用只在亮屏时运行）；观看端也能用。App 扩展里没有 `UIApplication`，在那里报告不可用。
- `rcsdk.clipboard`：`UIPasteboard`。读别的应用复制的内容时系统会弹“允许粘贴”，除非用户在设置里允许；查有没有变（`changeCount`）不弹；在后台读不了。剪贴板默认关，由宿主打开。

## 9. 分享端

**应用自己的画面**：编码器和 macOS 共用（指定“必须硬件”或“必须软件”的两个键要 iOS 17.4；17.0～17.3 上宿主明确要求时报 `UNSUPPORTED`）。采集来源只有 `RCS_CAPTURE_HOST` 和 `RCS_CAPTURE_ENCODED`，没有显示器列表。宿主推的画面：

- `rcs_session_push_gpu_frame`（`rcsdk/gpu.h`）：收 `CVPixelBuffer`（`rcs_capabilities.pushed_gpu_frames`），先核对类型、格式、缓冲区的实际布局、尺寸、颜色，再持有到编码完，不经 CPU。还没有流时是 `INVALID_STATE`。
- `rcs_session_push_sample_buffer`（`rcsdk/apple.h`）：ReplayKit 给的 `CMSampleBuffer` 原样交过来（见下）。
- 声音：`RCS_AUDIO_HOST`（ReplayKit 给的应用声音和麦克风，SDK 混成一路，见下）或 SDK 自己录的麦克风（`RCS_AUDIO_MICROPHONE`）；输入：`RCS_INPUT_HOST`。

**整个屏幕（广播上传扩展）**：iOS 只允许经 ReplayKit 的广播上传扩展录整个屏幕：用户在系统的面板上开始直播，系统在一个**单独的进程**里运行宿主的扩展，把屏幕（即使别的应用在前台）、设备放的声音和麦克风以 `CMSampleBuffer` 交给它。这个进程内存很紧（约 50 MB，超了就被系统杀掉），没有界面，也不是应用（没有 `UIApplication`）。

- **会话放在扩展里**：扩展是唯一一直在跑的进程（用户切到别的应用后，宿主应用很快就被挂起），所以分享会话在扩展里运行；宿主应用只负责控制面（配对、把设置交给扩展、打开系统的面板）。考虑过的另一种：扩展当助手、会话留在应用里（助手链接）——应用一挂起就断了，不行。
- **`rcs_session_push_sample_buffer`**：画面按 ReplayKit 的方向（`RPVideoSampleOrientationKey`，或宿主给的 `orientation`）在 GPU 上转正（`VTPixelRotationSession`），再按原来的形状放进流的尺寸、其余是黑的（`VTPixelTransferSession`），不经 CPU；流的信息里报告画面在哪里（`content_*`）、本来多大（`display_width` / `display_height`），转屏时 `RCS_EVENT_STREAM` 报告变化。声音：任意采样率、采样格式、字节序、一或两个声道的线性 PCM，设备和应用放的（`RCS_SAMPLE_BUFFER_AUDIO`）和麦克风（`RCS_SAMPLE_BUFFER_MICROPHONE`，和 ReplayKit 的 `RPSampleBufferType` 同值）各用自己的 AudioConverter 转成 48 kHz 立体声，再混成宿主的一路声音发出（见下一条）。镜像的方向是 `UNSUPPORTED`。它不只给 ReplayKit 用：macOS 上 ScreenCaptureKit、AVFoundation 给的也是 sample buffer。
- **两种声音混成一路**：在手机上分享屏幕的人通常一边演示一边说话，所以默认两种都发。`rcs_send_audio_config` 末尾的 `sample_buffer_audio_percent`、`sample_buffer_microphone_percent` 是两种声音各自的音量（0～400，默认都是 100）；0 表示不要这一种：它的缓冲照收（`RCS_OK`）、不转换、不发，另一种来了就发，不等它。所以“两种都要”“只要应用声音”“只要麦克风”都是这两个字段。运行中 `rcs_session_update` 改音量立即生效，声音不重启。混音在 `src/audio/mixer.cpp`（可移植，按两路 48 kHz 立体声浮点混），转换在 `apple/sample_buffers.mm`，接在 `session/host_sources.cpp` 的 `HostAudio` 上。
  - **按到达混，不比时间戳**：两种声音的缓冲各有各的长度和节奏（ReplayKit 常见的是应用声音 44.1 kHz 每块 1024 帧、约 23 ms，麦克风 48 kHz 每块 1024 帧、约 21 ms），时钟也不同。两种都在来时，一帧要等两种都到了才发出，快的那种等慢的，最多大约慢的一块，所以哪种都不断。时间戳不比：两种可能来自不同的时钟（宿主也可能把别处的麦克风推进来），对错了就整段错位，而按到达对齐的误差只有一块（约 20 ms），说话和应用声音之间听不出来。
  - **一种停了**：麦克风被关掉，或者应用都不出声（ReplayKit 这时可能就不再给这一种的缓冲）。另一种等它最多“它最近几秒里两块之间最长的间隔再加 20 ms”（还没见过间隔时 60 ms），之后照常发，停了的那种剩下的照样混进去；它再来就接着混。间隔超过 0.5 s 算停过，它之前剩下的已经过时、丢掉。按它自己的间隔判断，是因为成批到达的来源（例如每次三块、间隔 64 ms）不能被当成停了。等待只在停下的那一刻多出这几十毫秒，不会一直加在延迟上。
  - **时钟漂移和丢块不让延迟变长**：两种的时钟差几十 ppm，或者路上丢了一块，就有一种一直比另一种快，它等着的部分越积越多。每有一秒钟两种都在来，就看这一秒里每种发完后剩下的最少的量：那是它一直多出来的；到 10 ms 就从它前面去掉这么多，前后用 2 ms 交叉淡化接上（时钟差 100 ppm 时约 100 秒一次，日志里有一条）。每种最多留 1 秒。考虑过的另两种：给慢的一种加抖动缓冲、以快的为准（慢的会时断时续，还要多一个缓冲的延迟）；重采样追漂移（ppm 级的差用不着，扩展里的 CPU 和内存也紧）。
  - **加起来不超过满幅**：两种按音量相加后限制在 ±1。
  - 只推一种（例如宿主只要应用声音，干脆不推麦克风）时，它到了就发，不等另一种。
- **能在 App 扩展里运行**：没有 `UIApplication` 时不观察前后台；不用声音会话（声音来自 ReplayKit，不经 SDK 的音频引擎）；保持唤醒模块报告不可用。
- **内存**：分享会话本身在 Mac 上约 20 MB（1080p、20 Mbit/s）；扩展里画面在 GPU 上，不复制到 CPU。真机上还没有量。
- **宿主要做的**（QML 示例的 `examples/qml-demo/broadcast/` 是不带 Qt 的例子，写法见[指南 05](guide/05-quickstart-ios.md#分享端整个屏幕广播上传扩展)）：扩展的 `RPBroadcastSampleHandler` 读设置、第一帧来时按屏幕的形状定流的尺寸并启动会话，转屏时 `rcs_session_update` 换尺寸，用户暂停直播时 `rcs_session_pause`，出错时 `finishBroadcastWithError` 把原因显示给用户；设置经 App Group 或共用的钥匙串访问组交给扩展（免费的个人开发团队不能用 App Group，示例用钥匙串访问组）；`RPSystemBroadcastPickerView` 打开系统的面板；应用自己的分享会话先停，让扩展用同一个端口监听。

## 10. 测试

`ctest --preset ios-simulator` 在 iPad 模拟器里跑能在 iOS 上跑的全部测试，`ios_platform` 专测声音会话和音频引擎、前后台通知、剪贴板和保持唤醒模块（[BUILDING](BUILDING.md)）；`mixer` 用合成的 sample buffer（ReplayKit 的应用声音 44.1 kHz 大端 16 位立体声、麦克风 48 kHz 16 位单声道、不交错的 float32、16 kHz）测转换和两种声音的混音：不同的块长和节奏、一种停了又来、成批到达、时钟差 300～1000 ppm、丢一块、音量和不要某一种；会话场景 `sample_buffers` 经回环听到两种声音都在、运行中把麦克风的音量改成 0 后只剩应用声音。暂停的会话测试不依赖 iOS，在 Mac 和 Windows 的回环上也跑。模拟器没有硬件编解码器、ReplayKit 在模拟器里能开始录制但不出画面、广播开不起来，所以硬件编解码、应用画面和整个屏幕的分享（包括 ReplayKit 真实给的两种声音的格式、节奏和混音）、本地网络授权、AirPods 和来电打断都要在真机上验证。
