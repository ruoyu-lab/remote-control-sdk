# 示例程序

这里的程序只用来测试和演示 SDK，**不是 SDK 的一部分，也不代表任何产品**。它们只用公开接口（`include/rcsdk/`、官方模块和组件的头文件），可以参考写法。怎样在自己的程序里用 SDK，见 [docs/guide](../docs/guide/README.md)；构建见 [docs/BUILDING.md](../docs/BUILDING.md)。

`cmake --preset release && cmake --build --preset release` 编出 `rcsdk_cli`、`rcsdk_host_relay`、`rcsdk_unattended`（macOS、Windows）和 Windows 上的 `rcsdk_windows_viewer`，在 `build/release/bin/`。QML 示例要 Qt：`cmake --preset qt-release`。

| 目录 | 程序 | 演示什么 | 平台 |
| --- | --- | --- | --- |
| `cli/` | `rcsdk_cli` | 命令行的分享端和观看端，几乎每个功能都有一个选项：传输方式、身份、画质、画面格式、声音、控制、一对多、准入、模块、宿主的画面 / 码流 / 编解码器 / 传输、录制、统计 | 全部（iOS 模拟器、Android 经 adb） |
| `web/` | `viewer.html`、`forwarder.html`、`serve.mjs` | 浏览器观看端的测试页面（直连分享端；经转发器），和 node 写的小服务器（跨源隔离的头，把页面的 WebSocket 接到 `rcsdk_host_relay` 的房间，或经 `--forwarder` 替页面签令牌、把 WebSocket 转给转发器；默认只听环回，进房间要它打印的令牌），见[指南 23](../docs/guide/23-browser.md) | 预设 `wasm` 编出包；node |
| `host-relay/` | `rcsdk_host_relay` | 宿主自己的服务器的替身：按房间转发 `RCS_TRANSPORT_HOST` 的字节流（TCP）或数据报（UDP），也转发 P2P / 中继的信令（信令房间） | 除 iOS 外 |
| `windows/` | `rcsdk_windows_viewer` | Windows 原生观看端：在 SDK 的 D3D11 设备上用视频处理器把 GPU 帧画到交换链上，Win32 的键鼠转发 | Windows |
| `qml-demo/` | `RemoteControlQmlDemo` | 局域网远程控制的图形界面（Qt Quick）：设备发现、配对码、画质设置、GPU 帧直接画进场景图（Metal、D3D11、OpenGL ES）、单独传送的指针、剪贴板、保持唤醒；iPad / iPhone 上分享应用画面和整个屏幕（广播扩展），Android 上分享整个屏幕、经无障碍服务被控 | macOS、Windows、iOS、Android |
| `unattended/` | `rcsdk_unattended` | 无人值守：Windows 的服务 + 看护者（方案 A / B），macOS 的 LaunchDaemon + LaunchAgent | Windows、macOS |
| `pointer/` | （库 `rcsdk_example_pointer`） | 第三方模块的例子 `com.example.pointer`：观看端告诉分享端它指着哪里；只用 `rcsdk/module.h` | 全部 |
| `common/` | — | 共用的代码：HID 键码表（`keyboard.hpp`）、录制成文件（`recorder.hpp`）、经中继的宿主通道和信令（`relay_channel.hpp`、`relay_socket.hpp`、`signal_board.hpp`） | |

## rcsdk_cli

全部选项：`rcsdk_cli --help`。常用的几组：

```sh
# 生成密钥、看本机能做什么（编解码实现、显示器、权限）
rcsdk_cli --generate-key key.bin
rcsdk_cli --info
rcsdk_cli --list-windows

# 局域网：分享和观看（默认 UDP、H.264 硬件、1080p60）
rcsdk_cli --role share --address 192.168.1.10 --key-file key.bin --allow-control
rcsdk_cli --role view  --address 192.168.1.10 --key-file key.bin --seconds 30 --report view.json

# 预设、编码格式、画质要求、画面格式
rcsdk_cli --role share ... --preset low-latency --codec hevc,h264 --formats 444,420
rcsdk_cli --role view  ... --codec hevc,h264 --width 3840 --height 2160 --fps 120 --formats 444,420

# 没有硬件编解码器的机器（虚拟机、模拟器）：硬件优先，没有就用 SDK 自带的库
rcsdk_cli --role share ... --codec av1,vp9,h264 --acceleration hardware-first
rcsdk_cli --role view  ... --codec av1,vp9,h264 --acceleration hardware-first

# 软件编解码库：指定后端、画面内容、线程数（--backend openh264|libvpx|aom|dav1d）
rcsdk_cli --role share ... --codec vp9 --acceleration software --backend libvpx --content detail --software-threads 4

# Windows：4:4:4 的 HEVC 在 NVIDIA 显卡上由 NVDEC、Intel 核显上由 oneVPL 解（硬件优先时自动），Intel 核显上指定 oneVPL 编码
rcsdk_cli --role view  ... --codec hevc --formats 444,420 --acceleration hardware-first
rcsdk_cli --role share ... --codec hevc --formats 444 --backend onevpl

# 身份：口令、PIN、设备密钥
rcsdk_cli --role share --address 0.0.0.0 --pin 123456
rcsdk_cli --role view  --address 192.168.1.10 --pin 123456
rcsdk_cli --generate-device-key me.key                         # 打印公钥；对端用 --trust 公钥

# 公网：P2P / 中继，信令经 rcsdk_host_relay 的信令房间
rcsdk_cli --role share --transport p2p --signal-relay 203.0.113.7:47910 --stun stun.example.com:3478 --key-file key.bin
rcsdk_cli --role view  --transport p2p --signal-relay 203.0.113.7:47910 --stun stun.example.com:3478 --key-file key.bin
rcsdk_cli --role view  --transport relay --turn turn.example.com:3478 --turn-user U --turn-pass P --signal-relay ... --key-file key.bin

# 换网络（rcs_paths_config）：两端都打印路径事件（path changing / changed / lost / standby）
rcsdk_cli --role view ... --exclude-networks cellular --path-policy stay   # 不用蜂窝，不跟着默认网络来回
rcsdk_cli --role view ... --standby                                        # 另一块网络上备着一条路径
rcsdk_cli --role view ... --migrate off                                    # 换网络就断开重连（不迁移）

# 宿主自己的传输：经 rcsdk_host_relay
rcsdk_host_relay --listen 0.0.0.0:47910
rcsdk_cli --role share --transport host --relay 203.0.113.7:47910 --room demo --key-file key.bin
rcsdk_cli --role view  --transport host --relay 203.0.113.7:47910 --room demo --key-file key.bin --viewers 3

# 经转发器（server/forwarder，部署见 docs/guide/24-forwarder.md）：每张画面和声音上传一次
rcs_forwarder --generate-identity id.key                            # 打印公钥的 SHA-256（FWD）
rcs_forwarder --config forwarder.conf          # listen_udp、listen_tcp、listen_websocket、identity_key_file、secret = 1:HEX
                                               #（每一项：server/forwarder/deploy/forwarder.conf.example）
rcs_forwarder --sign-token --secret 1:HEX --room demo --role publish --identity id.key      # 分享端的令牌（TOKEN_S）
rcs_forwarder --sign-token --secret 1:HEX --room demo --role subscribe --route 7 --identity id.key   # 观看端的（TOKEN_V）
rcsdk_cli --role share --transport forwarder --address 203.0.113.7 --port 47995 --forwarder-key FWD --token TOKEN_S --key-file key.bin
rcsdk_cli --role view  --transport forwarder --address 203.0.113.7 --port 47995 --forwarder-key FWD --token TOKEN_V --key-file key.bin --viewers 3
rcsdk_cli --role view  --transport forwarder --forwarder-link tcp --address 203.0.113.7 --port 47995 ...   # 经它的 TCP 入口
rcsdk_cli --role view  --transport forwarder --forwarder-link wss://forwarder.example.com/rcs --forwarder-key FWD --token TOKEN_V --key-file key.bin
                                               # 经它前面的 TLS 代理（ws://HOST:PORT/rcs：直接连它的 WebSocket 入口；
                                               # --forwarder-pin HEX：代理是自签名证书时钉住它）
# 浏览器经转发器：serve.mjs 替页面签观看端的令牌，并把 /forwarder 的 WebSocket 转给转发器的 WebSocket 入口
node examples/web/serve.mjs --forwarder 127.0.0.1:47996 --forwarder-secret-file fwd.secret --forwarder-sha256 FWD
                                               # fwd.secret：一行 1:HEX，和转发器配置的 secret 相同；
                                               # 打开 http://127.0.0.1:8080/forwarder.html（docs/guide/23-browser.md 经转发器）

# 一对多和准入
rcsdk_cli --role share ... --max-viewers 8 --admit-after 5          # 每个观看端等 5 秒后放行
rcsdk_cli --role view  ... --viewers 4                               # 一个进程里开 4 个观看端
rcsdk_cli --role share ... --bitrate-from quorum|min|main --encodings auto|single|2   # 码率规则、低档编码
rcsdk_cli --role share ... --encodings single --layers 2              # 时域分层：跟不上的人先拿一半的帧

# 声音、模块、录制
rcsdk_cli --role view ... --send-audio microphone                   # 观看端说话
rcsdk_cli --role share ... --receive-audio on --exclude-own-sound
rcsdk_cli ... --clipboard both                                      # 两端都加
rcsdk_cli ... --clipboard both --clipboard-host --clipboard-text hi   # 程序自己的剪贴板，不碰系统的
rcsdk_cli ... --clipboard both --clipboard-files                    # 带文件：资源管理器、访达里复制，另一端粘贴
rcsdk_cli ... --clipboard both --clipboard-host --clipboard-files --clipboard-file a.pdf   # 程序自己的：发文件
rcsdk_cli ... --clipboard both --clipboard-host --clipboard-files --receive-dir ~/x       # 收到的文件取到 ~/x
rcsdk_cli --role share ... --receive-dir ~/Downloads ; rcsdk_cli --role view ... --send-file a.pdf
rcsdk_cli --role share ... --publish D:\test --publish-out id.txt   # 发布：对方取时才传，编号写进 id.txt
rcsdk_cli --role view ... --receive-dir ~/x --fetch id.txt          # 取进目录；--fetch-read 一段一段读（像系统粘贴）
rcsdk_cli --role view ... --record rec [--record-only]              # 写 rec.h264 / rec.ivf（VP8、VP9）/ rec.ogg……

# 电源：网络唤醒和远程电源操作（真的会发生）
rcsdk_cli --wake-info                                               # 本机能唤醒它的网卡，本进程能执行的操作
rcsdk_cli --wake 00:1b:21:3a:4b:5c --wake-address 192.168.1.255     # 发 3 个唤醒包，隔一秒
rcsdk_cli --role share ... --allow-control --power-actions restart,sleep --share-wake-info
rcsdk_cli --role view ... --power-ask restart                       # 分享端说了提供什么之后请求一次；,force 强制

# 不用屏幕：推测试画面（锁屏的机器、手机上的原生程序也能测按键到画面）
rcsdk_cli --role share ... --host-pattern --pattern-marker --allow-control --host-input --audio off
```

- 分享端的 `--agent-endpoint E --agent-key-file F` 把采集交给另一个进程里的助手，`rcsdk_cli --agent --agent-endpoint E --agent-key-file F` 运行那个助手；`--secure-desktop`（Windows，要 LocalSystem）、`--send-attention`（观看端发 Ctrl+Alt+Del）试无人值守。
- `--host-codec`（两端）用宿主自定义的格式 16，`--encoded-pattern`（分享端）直接推码流：演示 `rcsdk/host_codec.h`。
- `--log debug` 把 SDK 的日志打到标准错误，`--session-log LEVEL` 给这个会话自己的级别；`--report FILE` 在结束时写 JSON 统计。
- 启动失败时打印结果码和原因。

## rcsdk_host_relay

`rcsdk_host_relay [--listen 地址:端口]`（默认 `0.0.0.0:47910`，TCP 和 UDP）：一个房间一个分享端（`RCSSHARE <房间>`）、任意多个观看端（`RCSRELAY <房间>`），原样转发（TCP 是字节流，UDP 是数据报）；每个观看端有中继给的路由号，分享端那条连接上的每段字节前面带着它（TCP：路由号 8 字节、长度 4 字节；UDP：路由号 8 字节），观看端离开时分享端收到那个路由“关了”，分享端离开时观看端的连接被关掉（宿主据此调用 `rcs_session_transport_closed`）；信令房间（TCP 上 `RCSSIGNAL <房间>`）把每一行转给房间里的其他人，任意多个成员。宿主一侧的写法在 `common/relay_channel.hpp` 和 `common/signal_board.hpp`。两端都只向中继发起连接，不用为 SDK 开端口。没有账号、没有限制、不保存任何东西。

## rcsdk_windows_viewer

`rcsdk_windows_viewer --address IP --key-file key.bin [--port 47990] [--codec h264|hevc|hevc,h264]`。只用硬件解码；F8 开关控制，F9 切换适应 / 1:1，F11 全屏。

## RemoteControlQmlDemo

- **发现**：同一局域网内打开本程序的设备自动出现在“附近的设备”里（UDP 47901 广播）；也可以按 IP 连接（iPhone / iPad 真机收不到局域网广播）。
- **配对**：被控端开启“允许远程连接”后显示 6 位配对码，控制端输入后连接；连接密钥由配对码推导。这只是演示用的配对，不是产品级的方案。
- **被控端**：允许对方控制、画质设置（分辨率、帧率、码率上限、H.264 或 HEVC 优先、是否共享声音）；一次连接结束后自动重新等待。
- **控制端**：画面用 GPU 直接渲染、默认立即提交（`--vsync` 切回等刷新），顶部工具栏可以断开、开关控制、看统计、全屏。
- **iPad / iPhone**：被控端分享这个应用自己的画面（ReplayKit，输入在应用里执行），“共享整个屏幕”经广播扩展（`qml-demo/broadcast/`，设备的声音和麦克风由 SDK 混成一路）；控制端支持直接点按、触控板模式、软键盘、Apple Pencil。
- **Android**：分享整个屏幕和设备的声音（`qml-demo/android/` 的前台服务），经无障碍服务被控（在“设置 › 无障碍”里打开“远程控制 Demo：对方的操作”）。
- 自动化测试用的命令行参数：`--accept --code 123456`（直接开放），`--connect <IP> --code 123456`（直接连接），以及 `--port`、`--video-codec`、`--audio-codec`、`--resolution`、`--fps`、`--bitrate`、`--tcp`、`--allow-control`、`--log`。Android 上用系统属性给参数：`adb shell setprop debug.rcsdk.demo.args "--connect IP --code C"`。
- 构建：macOS、Windows 用预设 `qt-release`；iOS 和 Android 的步骤见 [docs/BUILDING.md](../docs/BUILDING.md)。

## rcsdk_unattended

- **Windows**：`install [--mode a|b] [--account system|user] [--os-session N] [--keep-awake none|system|display] [--power-actions LIST] [--share-wake-info] [--no-secure-desktop] [--auto-start] [--port N] [--address A] [--name NAME] [--dir DIR]`（管理员，注册服务）、`uninstall [--name NAME]`、`service [--console]`、`session`（方案 A 的被看护进程）、`agent`（方案 B 的被看护进程）、`sessions`（列出这台机器的会话）。密钥在 `--dir`（默认 `%ProgramData%\RcsdkUnattended`），`connection.key` 给观看端用。要 Ctrl+Alt+Del 先由管理员设置系统策略 `SoftwareSASGeneration`（示例不改）。
- **macOS**：`sudo rcsdk_unattended install [--port N] [--address A] [--keep-awake none|system|display] [--power-actions LIST] [--share-wake-info yes|no] [--label L] [--dir DIR] [--endpoint PATH]` 检查程序和目录只有 root 能改、生成密钥、写好并加载 LaunchDaemon 和 LaunchAgent 的 plist；`uninstall [--label L]`、`plists`（只打印 plist）、`session`、`agent`。助手要“屏幕录制”（远程输入还要“辅助功能”）授权。构建时以 `local.rcsdk.unattended` 为标识做开发签名。
- `--power-actions restart,shut-down,sleep,sign-out|all`：有控制权的观看端可以请求这台机器重启、关机、睡眠、注销（真的会执行，答复后 3 秒），日志记下每一步；`--share-wake-info` 告诉观看端网卡的 MAC 和子网，供网络唤醒。重启后要自动回来，Windows 上用 `--auto-start` 安装。
- 设计和要点见 [docs/guide/16-unattended.md](../docs/guide/16-unattended.md)。

## pointer

`pointer/pointer_module.{h,cpp}`：不到 100 行的第三方模块。注册 `example_pointer_module()`，两端在会话里用它（`rcs_module_config`，不带设置），观看端调用 `example_pointer_point`；分享端收到 `EXAMPLE_POINTER_MOVED` 事件。`rcsdk_cli --pointer`（两端都加）演示：观看端绕着屏幕中间画圈，分享端打印位置。写自己的模块见 [docs/MODULES.md](../docs/MODULES.md)。
