# 开发者指南

这份指南讲怎样**用** SDK 编写自己的程序，不需要读 SDK 的源码。接口的正式说明在头文件的注释里（`include/rcsdk/*.h`）；SDK 内部怎样实现见 [ARCHITECTURE](../ARCHITECTURE.md)，接口为什么是现在这个样子见 [API-DESIGN](../API-DESIGN.md)，每个版本改了什么见 [CHANGELOG](../../CHANGELOG.md)。本指南对应 0.1.5-beta（网络协议 v1）。1.0 之前每个版本都可能有破坏性修改。

## 先读

| 页面 | 内容 |
| --- | --- |
| [01 概览与基本概念](01-concepts.md) | 角色、会话、状态、核心配置和扩展链、预设、运行中修改；**最小的完整分享端和观看端（C 和 C++）** |
| [02 回调、线程、生命周期，以及从其他语言调用](02-threading.md) | 回调在哪个线程、回调里能做什么、谁拥有内存；Swift、C# 等语言要注意的 |

## 快速开始（按平台）

| 页面 | 内容 |
| --- | --- |
| [03 macOS](03-quickstart-macos.md) | 引入、权限、用 Metal 显示画面 |
| [04 Windows](04-quickstart-windows.md) | 引入（MSVC / DLL）、系统组件、用 D3D11 显示画面 |
| [05 iOS / iPadOS](05-quickstart-ios.md) | XCFramework、Swift、声音会话、分享应用画面、广播扩展分享整个屏幕 |
| [06 Android](06-quickstart-android.md) | AAR（Prefab）、`rcs_android_initialize` 和 JNI 桥、权限清单、OpenGL ES 显示、MediaProjection 和无障碍服务 |

## 功能

| 页面 | 内容 |
| --- | --- |
| [07 分享端](07-sharing.md) | 画质、编码、共享什么（显示器 / 区域 / 窗口 / 应用 / 缩略图）、多显示器、宿主自己的画面、一对多、准入、暂停 |
| [08 观看端](08-viewing.md) | 向分享端要什么、解码、GPU 帧和 CPU 帧、怎样画得快、坐标换算、指针、暂停、重连 |
| [09 传输、信令与公网](09-transports.md) | UDP / TCP / P2P / 中继 / 宿主的通道、`rcs1.` 信令、STUN 和 TURN、断线重连、数据通道 |
| [10 身份、密钥与安全](10-security.md) | 连接密钥、口令、短 PIN、设备密钥和配对、密钥存在哪里 |
| [11 远程控制与输入](11-input.md) | 授权、输入类型、键码、触屏、输入法 |
| [12 声音](12-audio.md) | 两个方向、格式、麦克风、回声、缓冲和延迟 |
| [13 预设、画面格式、动态码率与调优](13-tuning.md) | 预设、按场景配置、4:4:4 / 10 位 / HDR10、动态码率、调优扩展、实现的选择 |
| [14 事件、状态、统计与日志](14-events-stats-logging.md) | 全部事件、查询、统计分区、日志 |
| [15 模块](15-modules.md) | 剪贴板、文件传输、电源（保持唤醒、网络唤醒、远程重启 / 关机 / 睡眠 / 注销）、缩略图、文本插入点；写自己的模块 |
| [16 无人值守、助手进程与看护者](16-unattended.md) | 助手在另一个进程、Windows 的服务和看护者、macOS 的 launchd、安装清单 |
| [17 替换内部的一环](17-replacing.md) | 宿主的编解码器、编码包（录制）、宿主决定码率 |

## 参考

| 页面 | 内容 |
| --- | --- |
| [18 错误处理](18-errors.md) | 每个结果码的意思和宿主该怎么办 |
| [19 各平台的差别](19-platforms.md) | 最低版本、能力对照、权限 |
| [20 故障排查与常见问题](20-troubleshooting.md) | 连不上、启动失败、画面、声音、控制 |
| [21 版本、升级、分发与许可](21-versions-distribution.md) | 版本规则、引入方式、分发时带什么、第三方许可 |
| [22 示例与工具](22-examples-tools.md) | 用 `rcsdk_cli` 等程序测试自己的程序 |
| [23 浏览器观看端](23-browser.md) | 在网页里看和控：WebAssembly、WebCodecs、页面的通道 |
| [24 部署转发器](24-forwarder.md) | 构建、身份、配置、入口（UDP、TCP、WebSocket）、TLS 代理、令牌、Linux 的 systemd 服务和容器、macOS 的 launchd 和 Windows 的服务 |
| [reference/defaults.md](reference/defaults.md) | 每个结构体的默认值 |
| [reference/api-index.md](reference/api-index.md) | 按主题的接口索引 |

## 按任务找

- 把屏幕给对方看：[01](01-concepts.md#最小的完整程序) → [07](07-sharing.md)。
- 看、控对方的屏幕：[01](01-concepts.md#最小的完整程序) → [08](08-viewing.md) → [11](11-input.md)。
- 跨公网：[09](09-transports.md)；人多的群经转发器：[09](09-transports.md#经转发器rcs_transport_forwarder) → [24](24-forwarder.md)。
- 远程协助（PIN、先问用户再放行、对方说话）：[10](10-security.md#短-pin-的典型流程)、[07 准入](07-sharing.md#准入先问用户再放行)、[12](12-audio.md)。
- 无人值守的远程桌面：[16](16-unattended.md)。
- 录制和审计：[17](17-replacing.md#编码包交给宿主录制转发审计)。
- 嵌进游戏引擎或云渲染（宿主自己的画面、码流、编解码器）：[07](07-sharing.md#宿主自己的画面和声音)、[17](17-replacing.md)。
