# 更新记录

1.0 之前每个版本都可能有破坏性修改，不保留兼容（规则见 [docs/API-DESIGN.md](docs/API-DESIGN.md)）。升级时按这里的说明修改宿主代码、用新版本的头文件重新编译，并让两端运行同一个版本。

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
