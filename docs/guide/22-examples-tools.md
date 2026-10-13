# 22 示例与工具：用它们测试自己的程序

`examples/` 和 `tools/` 里的程序只用来测试和演示 SDK，不是 SDK 的一部分，也不代表任何产品。集成时最有用的是拿它们当“另一端”：自己的程序只写了观看端时，用 `rcsdk_cli` 当分享端，反过来也一样。每个程序的说明见 [examples/README.md](../../examples/README.md) 和 [tools/README.md](../../tools/README.md)。

| 想测的 | 用什么 |
| --- | --- |
| 自己的观看端 | `rcsdk_cli --role share --address <本机地址> --key-file key.bin --allow-control`；不想动屏幕时加 `--host-pattern`（推测试画面） |
| 自己的分享端 | `rcsdk_cli --role view --address <分享端地址> --key-file key.bin --seconds 30 --report view.json`（`--frames gpu|cpu` 看第一帧的布局和颜色） |
| 口令、PIN、设备密钥 | `--password`、`--pin`、`--secret none --device-key F --trust 公钥` |
| P2P、中继 | `rcsdk_host_relay` 当信令服务器：两端 `--transport p2p|relay --signal-relay 地址:端口`（`--turn …` 给中继） |
| 宿主自己的传输 | `rcsdk_host_relay` 当宿主的服务器：`--transport host --relay 地址:端口 [--relay-datagram]`；一个分享端、`--viewers N` 个观看端 |
| 转发器 | `rcs_forwarder --generate-identity id.key`（打印公钥的 SHA-256）、配置文件写 `listen_udp` / `listen_tcp` / `listen_websocket`、`identity_key_file`、`secret = 1:<64 位十六进制>`（每一项见 `server/forwarder/deploy/forwarder.conf.example`），`rcs_forwarder --config forwarder.conf` 运行并逐行打印房间和端的进出（`log_file` 写进文件；Windows 服务加 `--service`）；`rcs_forwarder --sign-token --secret 1:<十六进制> --room R --role publish|subscribe --identity id.key [--route N]` 签令牌（没有私钥文件时 `--audience <SHA-256>`）；两端 `rcsdk_cli --transport forwarder --address <转发器地址> --port <端口> --forwarder-key <SHA-256> --token <令牌>`，经 TCP 入口加 `--forwarder-link tcp`，经 WebSocket 用 `--forwarder-link ws://HOST:PORT/rcs` 或 `wss://...`（`--forwarder-pin`：代理的自签名证书）。部署（Linux 的 systemd 服务和容器、launchd、Windows 服务、TLS 代理）见 [24](24-forwarder.md) |
| 换网络 | 两端打印 `RCS_EVENT_PATH`（路线、两端地址、网卡、中断时长、是否经过信令）；`--migrate on|off`、`--exclude-networks cellular,…`、`--path-policy follow|stay`、`--standby`；`rcsdk_cli --info` 列出本机的网络和它们的种类、标志，以及连着的 Wi-Fi（频段、信道、信号、速率、AWDL 是否在别的信道上） |
| Wi-Fi 周期性卡顿 | 两端打印 `RCS_EVENT_STALLS`（多久断一次、每次多长，本端的 Wi-Fi）；观看端每秒的统计里有最近 10 秒的断流次数、中位和最长长度、节奏（[09](09-transports.md#wi-fi-周期性卡顿awdl)） |
| 一对多和准入 | 分享端 `--max-viewers N --admit-after S`，`--bitrate-from quorum|min|main`（码率规则），`--encodings auto|single|N`（低档编码），`--layers N`（时域分层）；观看端 `--viewers N`（一个进程里开 N 个）；分享端打印低档的启停（`stream 0 encoding 1: started …`）和观看端换档、换层（`viewer 2 gets encoding 1, 2 temporal layers (path, …)`），观看端的流信息带 `temporal layers 1 of 2` |
| 声音两个方向 | `--send-audio microphone|host`、`--receive-audio on|host` |
| 模块 | 两端 `--clipboard both`（`--clipboard-files` 带文件）、`--send-file` / `--receive-dir`、`--publish` / `--fetch` / `--fetch-read`、`--text-caret`、`--keep-awake`、`--pointer` |
| 网络唤醒、远程电源操作 | `rcsdk_cli --wake-info`、`--wake MAC --wake-address 广播地址`；分享端 `--power-actions LIST --share-wake-info`（真的会执行），观看端 `--power-ask ACTION[,force]` |
| 录制 | 观看端 `--record rec [--record-only]` |
| 本机能做什么 | `rcsdk_cli --info`（编解码实现和各自的画面格式、能力、权限、显示器、网络）、`--list-windows`、`--list-audio-devices` |
| 延迟 | `rcsdk_latency_target` + `rcsdk_response_probe`（按键到 SDK 交付画面）；`rcsdk_glass_probe`（屏幕到屏幕） |
| 编码器的时域分层、第二个编码会话 | `rcsdk_encoder_layers`（L1T1 对 L1T2、L1T3 的码率和 PSNR、第 0 层单独能不能解、第二个会话对主编码每帧时间的影响） |

- `rcsdk_cli --log debug` 把 SDK 的日志打到标准错误，每行写明来自会话、助手还是进程；失败时打印结果码和原因。
- `rcsdk_cli --report FILE` 在结束时把全部统计写成 JSON，是看每个统计字段实际含义的最快办法。
- 两端必须是同一个 SDK 版本：拿 `rcsdk_cli` 测试时用和自己的程序同一份源码编出来的。
- 移动端：iOS 模拟器里的 `rcsdk_cli` 见 [BUILDING](../BUILDING.md)（`ctest --preset ios-simulator`）；Android 上 `rcsdk_cli` 经 adb 推到 `/data/local/tmp` 运行（预设 `android-arm64`）。QML 示例在 iPad 和 Android 上两种角色都能试。
- 写自己的模块时，`examples/pointer/` 是只用公开头文件的完整例子。
