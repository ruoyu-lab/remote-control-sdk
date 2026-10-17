# 更新记录

1.0 之前每个版本都可能有破坏性修改，不保留兼容（规则见 [docs/API-DESIGN.md](docs/API-DESIGN.md)）。升级时按这里的说明修改宿主代码、用新版本的头文件重新编译，并让两端运行同一个版本。

## 未发布

- **修了**：Windows 上 Ctrl+Alt+Del（`RCS_INPUT_SECURE_ATTENTION`）没有效果。会话 0 里的发送者（看护者，或持有会话的服务）调用 `SendSAS` 之前先模拟了助手进程的令牌，想让它发到助手所在的会话（微软文档的说法）；在 Windows 11 上这样调用哪个会话都收不到，调用本身又不报错。现在不模拟、直接调用，发到控制台会话。被看护的进程在远程桌面会话里时，Ctrl+Alt+Del 不会到它那个会话。
- **修了**：Windows 被控时，注入的触摸要等手指移动或抬起才交给窗口：Windows 只在后续的帧到来时才把触点交出去，手指按住不动时什么也收不到，长按（例如长按出右键菜单）不起作用。现在有触点按着时，SDK 像触摸屏那样每 10 ms 再报一次（实测：首条消息在按下后 31～63 ms 到达，之前直到抬起都没有）。0.1.5-beta 修了位置之后，触摸在这台测试机上才到得了窗口。笔没有这个问题。

## 0.1.5-beta

macOS 上静止一阵后的第一下操作不再要等 100 多毫秒；Windows 上笔注入在扩展屏上的偏移修好了。网络协议仍是 v1，助手链接格式仍是 2；接口没变。

- **修了**：macOS 上，会话所在的应用不在前台、画面又静止了一阵时，系统让它“打盹”（App Nap）：SDK 的网络、采集和输入线程的定时器被推迟，静止后的第一下操作要等 100 多毫秒，过一阵才恢复。实测：一个不在前台的测试应用，静止 1 分钟后收到网络包要 164 ms，20 ms 的等待拖到 250 ms。现在会话运行期间、助手服务一个会话期间，SDK 向系统声明一项用户发起、对延迟敏感的活动（`NSProcessInfo` 的 `beginActivityWithOptions`，`NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical`），不阻止系统在闲置时睡眠（那是电源模块的事）。
- **修了**：Windows 被控、有一块显示器放在主显示器左边或上边时，笔（和触摸）注入的位置偏了：`InjectSyntheticPointerInput` 的坐标从虚拟桌面的左上角算起，不是屏幕坐标，SDK 却按屏幕坐标给了它。偏移正好是那块显示器的宽（或高），只有一块显示器、或其他显示器在右边下边时没有偏移。鼠标不受影响（它按整个虚拟桌面的比例注入）。
- **验证**：Mac 全量测试 192 项（`adaptive_tcp` 高负载时失败，单独重跑通过）；Windows、Android、iOS 模拟器、WebAssembly 交叉编译。笔：在 4K 主屏（150%）加左边 1080p 副屏的 PC 上，修复前偏 1920 像素，修复后两块屏幕上的每个测试点都准确。App Nap：后台测试应用三轮里一轮被系统休眠（对照组 164 ms，持有活动的一组 1 ms 以内），另两轮系统没有休眠它；真机上的远程会话里还没确认。

### 迁移

- 不用改代码。

## 0.1.4-beta

Mac 被控时，观看端的 Caps Lock 可以像 Mac 自己的键盘那样切换中英文。网络协议仍是 v1；助手链接格式改为 2（会话和另一个进程里的助手要一起升级）。

- **新增**：`rcs_control_config.caps_lock`（`rcs_caps_lock`）和 `caps_lock_hold_ms`，C++ `setCapsLock`。
  - `RCS_CAPS_LOCK_INPUT_SOURCE`（默认）：Mac 开了非拉丁输入法（拼音、韩文、日文等）时，短按在它和拉丁输入法（ABC）之间切换，各自是最近用过的那一个；按住 `caps_lock_hold_ms`（默认 500 ms）切大小写；大写锁定时按一下是关掉。没开非拉丁输入法时、以及其他系统上，只切大小写。
  - `RCS_CAPS_LOCK_CAPS_LOCK`：只切大小写，也就是之前的行为。
  - 原因：Mac 的"按一下切换中英文"在键盘驱动那一层，程序发的按键事件到不了，所以 SDK 用系统的输入法接口（Text Input Sources）自己执行。系统设置里"用 Caps Lock 键切换"这一项没有给程序读的接口，所以由宿主选。
  - 用户在 Mac 本机切换的输入法，经进程主线程的 run loop 才被看到；macOS 宿主本来就要让它转着。
- **示例**：`rcsdk_cli --caps-lock input-source|caps-lock`。
- **验证**：Mac 全量测试 192 项（`adaptive_tcp` 高负载时失败一次，单独重跑通过）；Windows 用 MSVC 编整个仓库并跑助手测试；Windows、Android、iOS 模拟器、WebAssembly 交叉编译；新的会话测试 `caps_lock`（设置传到注入器、运行中修改、无效值被拒绝）和助手链接的编解码测试；macOS 虚拟机里直接驱动注入器：拼音和 ABC 互切、按住切大小写、大写锁定时短按关掉、记住上次的非拉丁输入法（韩文）、`CAPS_LOCK` 模式。"没开非拉丁输入法"这一种在虚拟机里没测成（命令行进程禁用不了输入法），它和 `CAPS_LOCK` 走同一条路。

### 迁移

- `rcs_control_config` 多了两个字段，用 `RCS_CONTROL_CONFIG_INIT` 或 `rcs_control_config_init()` 初始化的代码重新编译即可。
- Mac 分享端的默认行为变了：Caps Lock 从只切大小写，变成在开了非拉丁输入法时切换输入法。要保持原来的行为，设 `caps_lock = RCS_CAPS_LOCK_CAPS_LOCK`。
- 用助手（`rcs_agent_link_config`）的宿主，会话和助手要用同一个版本：链接格式从 1 变成 2，版本不同时助手以 `UNSUPPORTED` 结束。

## 0.1.3-beta

Mac 被控时按 E、D 不再弹出表情面板、开始听写；P2P 优先走系统默认路由所在的网卡；QML 示例的横向滚动方向改对了。网络协议仍是 v1，助手链接格式仍是 1；接口没变。

- **修了**：Mac 被控时，观看端按过方向键、F 键、Home、End 之后，再按字母会带上 Fn（macOS 给这些键自带 Fn 标志，SDK 用的事件源把它留给了后面的按键）。Fn+E 打开表情与符号、Fn+D 开始听写，所以按 E、D 会弹出表情面板、开始语音输入。现在每个按键只带它自己的标志和观看端按住的修饰键。
- **改了**：P2P 收集本机地址时，系统默认路由所在网卡的地址优先级最高（之前只按 IPv6、IPv4 交替排，不看网卡）。两端经网线和 Wi-Fi 都能互通时，连接两个方向都走这块网卡；之前可能选中 Wi-Fi 上的地址，对端经 Wi-Fi 发进来，本机却经网线发出去，延迟跟着 Wi-Fi 走。
- **修了**：QML 示例的观看端，横向滚动方向反了：Qt 的 `angleDelta().x()` 正值是向左，`rcs_input_event.wheel_x` 正值是向右。照示例写观看端的宿主请同样取反。
- **验证**：Mac 全量测试 191 项全部通过；Windows、Android、iOS 模拟器、WebAssembly 交叉编译通过；真机上从 Windows 控制 Mac（方向键后按 E、D，网线和 Wi-Fi 同时连着），从 Mac 控制 Windows（横向滚动）。

### 迁移

- 接口没有变化。照示例的 `remote_video.cpp` 把 Qt 的 `angleDelta().x()` 直接当 `wheel_x` 的宿主，改成取反。

## 0.1.2-beta

Mac 分享时打字后指针不再消失；QML 示例在新版 Qt 的 Windows 上方向键不再变成小键盘键。网络协议仍是 v1，助手链接格式仍是 1；接口没变。

- **修了**：Mac 分享时，在 Mac 上打字（本机或观看端发来的按键）会让 macOS 把指针藏到鼠标再动为止，SDK 原样报成"看不见"（`rcs_cursor_event.visible` 为 0）。观看端发来的移动不算鼠标动了，指针就一直不回来，观看端也一直藏着它。现在按键那一刻藏起来的指针照旧报"看得见"；程序真的藏指针（全屏视频、游戏）不受影响。
- **修了**：QML 示例在 Qt 6.9 及以后的 Windows 上，方向键、Home、End 等扩展键被当成了小键盘的键（在 Mac 上打出 4、8、6、2）。Qt 6.9 起扩展键的扫描码是 `0xE0xx`，不再用第 8 位表示。按示例的 `qt_input.hpp` 换算按键的宿主请同样修改。
- **验证**：Mac 全量测试 191 项里 190 项通过（`adaptive_tcp` 是高负载时的偶发失败，单独重跑通过）；真机上从 Windows 观看并控制 Mac：打字后指针一直在，方向键正常。

### 迁移

- 接口没有变化。自己从 Qt 的 `nativeScanCode()` 换算 Windows 按键的宿主，改成同时认 `0xE0xx` 和第 8 位两种扩展键写法（见示例的 `qt_input.hpp`）。

## 0.1.1-beta

修了用 Xcode 生成器编译 iOS 时缺符号的问题。网络协议仍是 v1，助手链接格式仍是 1；接口没变。

- **修了**：Apple 平台上，SDK 的库同时编了两个同名源文件，`src/platform/apple/network.mm` 和 `src/platform/other/network.cpp`。Ninja、Makefile 按完整路径给目标文件起名，不受影响。Xcode 生成器（iOS 宿主的工程常用）把一个库的目标文件放在同一个目录、只按文件名起名，结果两个都叫 `network.o`，后一个覆盖了前一个，链接时缺符号。改法：
  - 后一个改名为 `platform/other/wifi_power.cpp`；
  - Windows 库里同样隐患的三对也改了名：`gdi_cursor`、`thumbnails`、`tls_connection`；
  - CMake 现在会检查：同一个库里出现同名源文件，配置时直接报错并说明是哪个。
- **验证**：Mac 全量测试 191 项里 188 项通过，另外三项（`udp`、`session.sample_buffers`、`session.text_caret`）是机器高负载时的偶发失败，都不涉及改名的文件，单独重跑通过；用 Xcode 生成器编 iOS 模拟器版（`cmake -G Xcode -DCMAKE_SYSTEM_NAME=iOS`）通过；Windows、Android、iOS 模拟器、WebAssembly 交叉编译通过。

### 迁移

- 接口没有变化。宿主为绕过这个问题加在自己构建里的临时代码可以删掉。

## 0.1.0-beta

第一个公开版本。网络协议 **v1**，信令格式 1，助手链接格式 1，各官方模块的协议版本 1，转发器令牌 `rcsf1.`。

### 包含什么

- **四个系统**：macOS 和 Windows 两种角色都能当；iOS / iPadOS 观看，分享应用自己的画面或经广播扩展分享整个屏幕（系统不允许被控）；Android 观看、分享整个屏幕，经宿主的无障碍服务被控。浏览器（WebAssembly）观看和控制，经转发器或宿主自己的通道。Linux 上只有转发器服务器。同一套接口，系统做不到的由 `rcs_capabilities` 说明，调用以 `RCS_ERROR_UNSUPPORTED` 和原因失败。
- **画面**：
  - 编码格式：H.264、HEVC、VP8、VP9、AV1，两端协商。
  - 硬件：NVENC、NVDEC、oneVPL、Media Foundation、VideoToolbox、MediaCodec。软件：随 SDK 编译的 OpenH264、libvpx、libaom、dav1d。
  - 画面规格：4:4:4、10 位、HDR10；HEVC、AV1 到 8K。
  - 共享什么：显示器、区域、窗口、应用，多显示器每块一路。也可以推宿主自己的画面和码流，或用宿主自己的编解码器。
  - 帧率按编码器在这个画面格式下的上限降低（`max_pixels_per_second`）。
- **声音**：两个方向（分享端的声音、观看端的麦克风），Opus、AAC、PCM，系统的回声消除。
- **连接**：
  - 传输方式：UDP、TCP、P2P（ICE，宿主转发信令）、TURN 中继（UDP、TCP、TLS）、多人用的转发器（UDP、TCP、WebSocket 入口），或宿主自己的通道。
  - 安全：端到端加密、前向保密。
  - 抗弱网：动态码率、FEC、参考帧失效、长期参考帧、断线重连。
  - 网络变化（换网络、NAT 重新映射、TCP 断开）时保持连接，可选备用路径。
- **身份**：连接密钥、口令、短 PIN（PAKE）、设备密钥，准入由宿主决定。
- **控制**：键鼠、平滑滚动、文字和输入法、媒体键、触摸和笔，按观看端授权，可以交给宿主执行。
- **一对多**：一份编码发给最多 256 个观看端；给弱网的观看端低档编码和时域分层；转发器把分享端的上传降到一份。
- **模块**：剪贴板（含文件）、文件传输（断点续传）、电源（保持唤醒、网络唤醒、远程电源操作）、缩略图、文本插入点；第三方模块用同一套接口。
- **无人值守**：Windows 的服务和看护者（登录屏、锁屏、UAC），macOS 的 LaunchDaemon + LaunchAgent。
- **诊断**：事件、分区统计和日志回调；路径上周期性断流的检测（`RCS_EVENT_STALLS`）和本机 Wi-Fi 的信道、AWDL 状态（`rcs_list_wifi`）；运行中编码器打不开的改动只回绝那一次（`RCS_EVENT_ENCODING` 的 `encoding` 0），流照旧。
- **分发**：C 接口和纯头文件的 C++ 包装、CMake 安装包、iOS 的 XCFramework、Android 的 AAR、浏览器的 npm 包、转发器的 systemd 单元和容器。

### 验证到哪里

- 每次改动都在 Mac（M4 Pro）上跑全部测试；Windows（RTX 4090 + Intel 核显）用 MSVC 编整个仓库并跑编解码测试；Android 模拟器和 Galaxy S25+、iPad 模拟器跑各自的测试；转发器在 GitHub Actions 的 Linux 作业里测试。
- 真机上验证过哪些、只实现了哪些、还在计划里的，见 [ROADMAP 的“当前状态”](docs/ROADMAP.md)。实测数字写明了设备、分辨率、码率和测量终点，见 [PERFORMANCE](docs/PERFORMANCE.md)。

### 迁移

- 第一个公开版本，没有要迁移的。之前的内部构建都不能和它互连，两端和转发器一起换成 0.1.0-beta。
