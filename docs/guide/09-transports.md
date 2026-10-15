# 09 传输、信令与公网

传输方式是核心配置的 `connect.transport`，两端必须一致，在会话的一生中不变。所有传输上都是同一个端到端加密的连接（握手、AES-256-CTR + HMAC-SHA256），中继和宿主的服务器只看得到密文。

| 传输方式 | 适用场景 | 宿主要做的事 |
| --- | --- | --- |
| `RCS_TRANSPORT_UDP`（默认） | 局域网，或端口可达的环境 | 分享端填本机地址和端口，观看端填对方的。视频按需重传、发送节流、过时就丢，输入不排在视频后面，码率随网络调整 |
| `RCS_TRANSPORT_TCP` | 只放行 TCP 的网络 | 同上。走的是和 UDP 一样的数据报，只是在观看端连过去的 TCP 连接里：丢一个包，后面的画面和输入都要等 TCP 重传；换网络时观看端再连一条，会话接着走 |
| `RCS_TRANSPORT_P2P` | 公网直连：家庭宽带、手机热点、IPv6 | 转发两端的信令文本；建议配置 STUN 服务器 |
| `RCS_TRANSPORT_RELAY` | 直连打不通时；封 UDP 的网络 | 转发信令文本；配置自己部署的 TURN 服务器和凭据 |
| `RCS_TRANSPORT_HOST` | 宿主已经有一条通到对方的通道：自己服务器的长连接、WebRTC 数据通道、QUIC、SSH 隧道 | 把 SDK 给的数据按路由号交给自己的通道，把收到的带着路由号交回 SDK；两端由宿主配对，分享端可以有多个观看端 |
| `RCS_TRANSPORT_FORWARDER` | 人多的群：分享端的上行不随人数增长；局域网和公网 | 部署转发器（`rcs_forwarder`，[24](24-forwarder.md)），宿主的服务器给每一端签令牌；两端填转发器的地址（UDP、TCP）或 WebSocket 的 URL、公钥指纹和令牌 |

## UDP 和 TCP

- 分享端绑定 `connect.address`（数字的 IPv4 或 IPv6；`0.0.0.0` 是所有 IPv4 网卡）和 `connect.port`（默认 47990），观看端连接对方的地址和端口。
- 防火墙要放行分享端的这个端口（Windows 防火墙按程序路径放行，第一次监听时弹确认框）。
- 分享端在 `LISTENING` 时一直等到有连接通过握手：密钥不对、协议版本不同、连上不说话的连接只会被丢弃（计入网络统计的 `rejected_connections`），不会结束分享会话。
- P2P 收集本机地址时，系统默认路由所在网卡（有线和 Wi-Fi 并存时通常是网络服务顺序里靠前的那块）的地址优先级最高，所以两端都能经几块网卡互通时，连接两个方向都走这块网卡，不会出现对端经 Wi-Fi 发进来、本机经网线发出去的情况。
- 有线和 Wi-Fi 并存、或者装了 VPN 时，`rcs_network_tuning_config.local_interface` 把会话限定在一块网卡上（[13](13-tuning.md#网络)）；按网络种类挑（例如不用蜂窝）用 `rcs_paths_config`（见下面的[网络变化](#网络变化连接迁移)）。
- UDP 和 TCP 的分享端监听通配地址（`0.0.0.0` / `::`）时，换了地址的观看端还能找到它（观看端连的是分享端的地址）；分享端监听一个具体地址时，那个地址没了就只能重连。
- TCP 上跑的是和 UDP 一样的连接：每个数据报（最大 16 KiB）前面 2 字节长度，视频分片、选择性重传、发送节奏、动态码率、声音和输入不排在视频后面，都和 UDP 一样；不同的是 TCP 自己按序重传，丢一个包后面的都要等它（队头阻塞）。观看端的每条 TCP 连接是这个连接的一条路径：断了（或网络变了）就再连一条，分享端的端口按连接编号把它交给原来的连接（[网络变化](#网络变化连接迁移)）。分享端的端口上没认领的 TCP 连接同时最多 16 条（再来一条，最早的那条关掉），连上 2 秒不发握手的关掉；一个连接最多 4 条（多出来的，最久没往上发过东西的那条关掉）。

## P2P 和中继：信令

两台机器先交换一次“我在哪、怎么找到我”：SDK 通过 `on_signal` 给宿主一段文本，宿主用自己的任何通道（自己的服务器、已有的消息系统、HTTP 接口……）交给对方，对方调用 `rcs_session_signal` 交回 SDK。之后 SDK 用 ICE（RFC 8445）检查每一对地址，选定一条双方都能通的路径，再在上面握手。这能覆盖局域网、IPv6，以及大多数家庭宽带和手机网络的 NAT。

- **观看端先给**，每次连接一份（重连时再给一份新的）；它的 `on_signal` 的 `route` 总是 0，它的 `rcs_session_signal` 不看 `route`。
- **分享端从不主动给**，而是回答每一份观看端信令，一份对一份。宿主把观看端的信令交给分享端时带上自己的路由号 `route`（`uint64_t`：这份信令从哪来，用户号、连接号、一个指针，SDK 不解释），分享端的回答从 `on_signal(text, route, user_data)` 带着同一个 `route` 出来，宿主照它送回去，不用读文本。观看端加入后，`rcs_viewer_info.route` 就是它的 `route`。
- 宿主不需要配对、去重或判断新旧：重复送达的、乱序的、对端以前的连接留下的信令都可以照常交给 `rcs_session_signal`。分享端的回答写明它回答的是哪一份观看端信令，观看端的连接只用回答自己的那份。在 `start` 之前交进来的，留给下一次运行。
- 观看端等分享端的回答：第一次连接一直等到 `stop`（宿主自己决定等多久），重连时只等到重连时间窗结束。
- **运行中也会有信令**：连接建立以后，任一端的路径要重新打洞时（换了网络、路径断了、备用路径），`on_signal` 随时会再给出一份“重启”信令（分享端的带着那个观看端的 `route`，观看端的 `route` 是 0），对端照样交给 `rcs_session_signal` 并回答。宿主在会话期间保持信令通道，按和第一次一样的方式转发（[网络变化](#网络变化连接迁移)）。

### 信令文本的格式

宿主转发信令时不需要解析它，但要知道它的样子，好给通道留够大小：

- 纯 ASCII：前缀 `rcs1.`，后面是不带填充的 base64url（`A`–`Z`、`a`–`z`、`0`–`9`、`-`、`_`）。前后的空格和换行会被忽略。可以放进 JSON 字符串、URL 参数、短信以外的绝大多数消息里。
- 内容：格式版本（0.1.2-beta 是 1；两端 SDK 版本不同时可能对不上，`rcs_session_signal` 以 `INVALID_ARGUMENT` 说明“unknown version”）、种类、角色、本次连接的 ICE 用户名片段和口令（一次性的随机十六进制，用来认证连通检查）、分享端回答的是哪一份、最多 32 个候选地址（地址、端口、类型、优先级）。**不含**共享密钥、口令或设备密钥。但候选地址暴露了本机的局域网和公网地址，宿主转发时按隐私数据对待。
- 种类：连接的第一次交换；运行中的“重启”（多一个 8 字节的连接标记、代数、是否备用路径，最后是用连接密钥派生的 16 字节 MAC：宿主转得了，改不了也造不出，改过的被 `rcs_session_signal` 以 `RCS_ERROR_AUTHENTICATION` 拒绝）；“没有这个连接”（只有连接标记：对方已经没有这个连接，比如它重启过，观看端据此马上照旧重连）。
- 大小：观看端的信令约 67 个字符加上每个 IPv4 候选 16 个、每个 IPv6 候选 32 个字符；分享端的多约 11 个字符；重启信令再多约 40 个字符，“没有这个连接”约 20 个字符。常见的几个候选是 150～300 个字符；上限（32 个 IPv6 候选）约 1150 个字符。宿主的通道至少要能带 1200 字节的一条消息。比任何信令都长的文本（约 1420 个字符以上）不解码，直接 `INVALID_ARGUMENT`。
- 对端信令里的候选地址都会被探测（连通检查、TURN 的许可），所以收到的一端剔除没有对端会在的地址（未指定、组播、广播、保留地址、云元数据服务），环回和链路本地地址只在本端自己也有同一范围的候选时才用，同一个 IP 最多取 4 个；剔除的写日志，失败原因里也列出来。同一台机器上不经网络测试：两端都设 `rcs_network_tuning_config.ice_addresses = RCS_ICE_ADDRESSES_LOOPBACK`（[13](13-tuning.md)）。

```cpp
// 观看端：它的信令送给分享端，分享端的回答交回来
rcsdk::Callbacks viewCallbacks;
viewCallbacks.onSignal = [&](const std::string& text, uint64_t) {
    channel.send(sharer, text);   // 宿主自己的通道；任何语言、任何后端都行
};
rcsdk::Session view(rcsdk::SessionConfig(RCS_ROLE_VIEW)
                        .setKey(key)
                        .setTransport(RCS_TRANSPORT_P2P)
                        .setStunServers({"stun.example.com:3478"}),
                    std::move(viewCallbacks));
view.start();
channel.onMessage([&](const std::string& text) { view.signal(text); });   // 分享端的回答

// 分享端：route 是宿主自己的号，这里用发信人的用户号
rcsdk::Callbacks shareCallbacks;
shareCallbacks.onSignal = [&](const std::string& text, uint64_t route) {
    channel.send(UserId(route), text);   // 回答送回那个观看端
};
rcsdk::Session share(rcsdk::SessionConfig(RCS_ROLE_SHARE)
                         .setKey(key)
                         .setTransport(RCS_TRANSPORT_P2P)
                         .setStunServers({"stun.example.com:3478"}),
                     std::move(shareCallbacks));
share.start();
channel.onMessage([&](UserId from, const std::string& text) { share.signal(text, from.value()); });   // 观看端的信令
```

信令服务器可以非常简单：把一个房间（一次分享）里的消息转给其他成员。示例 `rcsdk_host_relay` 的“信令房间”就是这样一个替身（`RCSSIGNAL <房间>` 之后每一行转给房间里的其他人，`examples/common/signal_board.hpp` 是客户端一侧），`rcsdk_cli --signal-relay` 用它（[22](22-examples-tools.md)）。

### 直连失败

**直连失败不会自动改走中继**。连通检查在 `ice_connect_ms`（`rcs_network_tuning_config`，默认 15 秒）内找不到路径时，本次运行以 `RCS_ERROR_NETWORK` 失败，`rcs_session_get_failure` 说明检查了哪些地址、有几对得到回应。是否改用 `RCS_TRANSPORT_RELAY` 由宿主决定（打算这样做的宿主可以把 `ice_connect_ms` 调短，早点知道），两端都要换：

```cpp
if (session.failure().result == RCS_ERROR_NETWORK && transport == RCS_TRANSPORT_P2P) {
    config.setRelay("turn.example.com:3478", turnUser, turnPassword);   // 两端都换成中继，重新开始
}
```

## STUN 和 TURN 服务器：`rcs_ice_servers_config`

都由宿主部署，SDK 不内置任何第三方服务器。

- `stun_servers[]`（最多 4 个，`"host:port"`，名字、IPv4 或 `[IPv6]`）：P2P 用，告诉每一端它的公网地址；解析不了或不回答的被跳过（写日志）。没有 STUN 时只提供本机网卡的地址（局域网、IPv6）。
- `turn_server`（`"host:port"`，不写端口时 3478，TLS 时 5349）、`turn_username`、`turn_password`：中继用的标准长期凭据。生产环境建议由宿主后端按会话签发有时效的凭据（coturn 的 `use-auth-secret`）；凭据被拒（例如过期）是 `RCS_ERROR_AUTHENTICATION`。配了用户名时服务器必须先要凭据，没要就批下的分配不接受（路径上的人也能这样回答），同样是 `AUTHENTICATION` 并说明；不认证的服务器把用户名留空。
- `turn_transport`：到 TURN 服务器怎么走。`RCS_TURN_UDP`（默认）；`RCS_TURN_TCP`（公司、酒店只放行 TCP 时；coturn 加 `alt-listening-port=443` 更容易通过）；`RCS_TURN_TLS`（只放行 HTTPS、检查内容的网络，见下）。两端可以各走各的，中继在两端之间转发的始终是 UDP 数据报。在一次运行中固定。
- 推荐部署开源的 coturn，它同时提供 STUN 和 TURN。中继只转发密文。每个观看端在 TURN 服务器上有自己的一个分配（coturn 的 `max-bps` 按分配限速，就是按观看端限速）。
- 服务器和凭据可以运行中修改（`rcs_session_update`），从下一个连接起生效，例如给常驻监听的分享端换上新的有时效凭据。

**TURN over TLS**（`RCS_TURN_TLS`）：到中继是 TLS 1.2 起（Windows 11 起有 TLS 1.3），里面和 TURN over TCP 一样。用系统的 TLS（macOS / iOS 的 Network.framework，Windows 的 Schannel，Android 用编进 SDK 的 BearSSL），提供应用协议 `stun.turn`（ALPN），中继和网站共用 443 时前面的 ALPN 分流可以据此把连接交给 coturn。服务器证书怎么校验，宿主二选一：

```cpp
// 默认：系统信任的证书，名字是 turn_server 的主机名（或 turn_tls_server_name）
config.setRelay("turn.example.com:443", turnUser, turnPassword, RCS_TURN_TLS);
// 自建、自签名的中继：只认这张证书（DER 的 SHA-256：openssl x509 -outform der | openssl dgst -sha256）
config.setRelay("203.0.113.7:443", turnUser, turnPassword, RCS_TURN_TLS).setRelayCertificate(sha256);
```

- 给了指纹（`turn_certificate_sha256`）就只认这一张证书，不看信任链、名字和有效期；证书续期后指纹会变，宿主要同时更新。公司里检查 TLS 的代理（它的根证书装进了系统）能通过系统信任的校验，但永远通不过指纹校验。
- 没给指纹时用系统的信任存储和名字（`turn_tls_server_name`，空就用 `turn_server` 的主机名）；`turn_server` 是 IP 地址又没给名字和指纹时是 `INVALID_ARGUMENT`。不做在线吊销检查，服务器要自己发出中间证书。
- Android 上“系统的信任存储”是系统自带、用户没有停用的根证书，不含用户自己安装的证书（设置 › 安全 › 加密与凭据）：Android 7 起应用默认也不信任它们，除非应用的网络安全配置（network security config）写明信任，而 SDK 读不到这份配置；被诱导装进去的证书不能冒充中继。用私有 CA 或用户安装的证书的中继，宿主用指纹（`turn_certificate_sha256`）。
- 指纹或名字配在别的 `turn_transport` 上是 `INVALID_ARGUMENT`（配了却没人检查，等于没有校验）。
- 证书不对时本次连接以 `RCS_ERROR_AUTHENTICATION` 结束，原因写明哪一项：指纹不符、链不受信任、名字不符、过期或未生效。服务器根本不说 TLS、连不上是 `RCS_ERROR_NETWORK`。

## P2P 和中继上的多个观看端

分享端为每一份观看端信令单独开一次“尝试”：新收集一次候选地址（中继：在 TURN 服务器上新分配一个），给出一份只回答这份信令的信令，在这一对之间做连通检查和握手。之后这个观看端就和 TCP / UDP 上的一样（[07 一对多](07-sharing.md#一对多)）。

1. 分享端 `start` 后马上进入 `LISTENING`，不收集、不给信令。
2. 观看端 `start`（或重连）时收集候选地址，`on_signal` 给出信令，宿主把它送到分享端：`rcs_session_signal(share, text, route)`。
3. 分享端为它收集（有 STUN 时约一个往返，最多 `ice_gather_ms`，默认 1.5 秒；中继：分配，约两个往返），然后 `on_signal(answer, route)`；宿主把回答送回那个观看端：`rcs_session_signal(view, answer, 0)`。
4. 两端做连通检查、握手；分享端判断能不能服务（满员、格式、尺寸、声音：`BUSY`），问 `on_viewer_join`，加入。

- 同时进行的尝试最多 32 个，再来的信令排队（最多 256 份，满了丢掉最早的一份并写日志）。同一份信令再送来不再开尝试；以前的连接留下的信令也会被回答，但没人用，那次尝试到 `ice_connect_ms` 超时结束（只写日志）。
- **端口**：每个观看端的路径有自己的 UDP 端口（中继：自己到 TURN 服务器的 UDP 端口或 TCP / TLS 连接，以及自己的分配）。`connect.port` 为 0 时由系统选；不为 0 时，每次尝试从 `connect.port` 开始往上取第一个空闲的，最多往上找 `max_viewers + 32` 个，防火墙放行这一段就够了。
- **失败**：一次尝试失败（连通检查不通、握手失败）时，本次运行还没有观看端加入过，常驻监听的分享端回到 `LISTENING`、状态事件带着原因；已经有观看端时只写日志，其他观看端不受影响。分享端这一侧的失败（TURN 服务器解析不了、凭据被拒、指定的网卡不存在）在还没有观看端时结束运行，已经有观看端时只让这一次尝试失败。

## 宿主自己的传输：`RCS_TRANSPORT_HOST`

宿主已经有一条通到对方的通道时，SDK 的整条连接可以走它：握手、端到端加密、画面、声音、输入、数据通道，以及选择性重传、发送节流、动态码率、常驻监听和自动重连，都和 UDP 一样。头文件 `rcsdk/host_transport.h`（C++ `rcsdk/host_transport.hpp`）。

1. **配置**：`connect.transport = RCS_TRANSPORT_HOST`，再放一个 `rcs_host_transport_config`（`RCS_EXT_HOST_TRANSPORT`；两者缺一个时创建会话返回 `INVALID_ARGUMENT`）：`host_channel` 说明通道是数据报（`RCS_HOST_CHANNEL_DATAGRAM`）还是字节流（`RCS_HOST_CHANNEL_STREAM`），`host_max_datagram` 是本端发出的最大数据报（0 是 1200，范围 `RCS_HOST_MIN_DATAGRAM`（1136）..65535）。在一次运行中固定。
2. **路由号**：分享端的通道通到每个观看端各一条，宿主给每条起一个自己的编号 `route`（`uint64_t`：服务器上的连接号、用户号、一个指针，SDK 不解释），收发都带着它；观看端的通道只通到分享端，`route` 总是 0（别的值是 `INVALID_ARGUMENT`）。
3. **发出去**：`on_transport_send(route, data, size, user_data)` 把这些字节经 `route` 那条通道发给对端（C++ 实现 `rcsdk::HostTransport::send(route, data, size)`）。在连接的线程上调用，同一个 `route` 一次一个（分享端不同 `route` 的调用可能同时发生），**不能阻塞**：通道这会儿收不下就返回 0（背压），SDK 当作丢了，重传该重传的，动态码率随之下降（`rcs_network_stats.host_refused` 计数）。字节流要么全收、要么全不收。
4. **收进来**：`route` 那条通道收到的字节交给 `rcs_session_transport_receive(session, route, data, size)`（任何线程；数据报一次一条，可以同时调用；字节流每个 `route` 按顺序、一次一个调用）。`route` 那条通道断了（数据通道关闭、WebSocket 断开、服务器那头换了人）时调用 `rcs_session_transport_closed(session, route)`；换了一条新的、仍通到同一个对端（WebSocket 断了又连上、WebRTC 重新做了 ICE）时调用 `rcs_session_transport_restarted(session, route)`，连接接着走。

```cpp
#include <rcsdk/host_transport.hpp>

// 宿主自己的通道（例如到自己服务器的 WebSocket）包成 HostTransport
class ChannelTransport final : public rcsdk::HostTransport {
public:
    explicit ChannelTransport(MyChannel& channel) : channel_(channel) {}
    bool send(uint64_t, const uint8_t* data, uint32_t size) override {   // 观看端：route 总是 0
        return channel_.trySend(data, size);   // 满了就返回 false，不要等
    }

private:
    MyChannel& channel_;
};
ChannelTransport transport(channel);   // 活得比会话久
rcsdk::Session session(rcsdk::SessionConfig(RCS_ROLE_VIEW)
                           .setKey(key)
                           .setTransport(RCS_TRANSPORT_HOST)
                           .use(rcsdk::hostTransport(transport, RCS_HOST_CHANNEL_DATAGRAM)));
channel.onMessage([&](const uint8_t* data, size_t size) { rcsdk::transportReceive(session, 0, data, uint32_t(size)); });
channel.onClose([&] { rcsdk::transportClosed(session, 0); });
session.start();
```

分享端的写法一样，只是 `send` 按 `route` 交给那个观看端的通道，收到时带上它来自哪条通道：

```cpp
class RoomTransport final : public rcsdk::HostTransport {
public:
    bool send(uint64_t route, const uint8_t* data, uint32_t size) override {
        return room_.trySendTo(route, data, size);   // route 是宿主自己的号，例如服务器上那个观看端的连接号
    }
    // ...
};
room.onMessage([&](uint64_t from, const uint8_t* data, size_t size) {
    rcsdk::transportReceive(sharing, from, data, uint32_t(size));
});
room.onLeave([&](uint64_t from) { rcsdk::transportClosed(sharing, from); });
```

| 宿主的通道 | 用法 |
| --- | --- |
| WebRTC 数据通道 | `ordered: false, maxRetransmits: 0`，DATAGRAM，`host_max_datagram` 设 1136：SCTP 一个包带的数据约 1150～1170 字节，1200 的消息会被拆成两片，不可靠模式下丢一片整条就丢。可靠模式的数据通道也能用，延迟会被它的重传拖住 |
| QUIC | DATAGRAM 帧：DATAGRAM，取连接允许的最大数据报（不低于 1136）；或者一条双向流：STREAM |
| 经宿主服务器的 WebSocket | 二进制消息：DATAGRAM，可以设大（例如 16384，消息条数少很多）；服务器只按房间或会话转发。关掉 `permessage-deflate`：内容是密文，压缩不了 |
| TCP、TLS、SSH 隧道 | STREAM：SDK 在流里给每个数据报加 2 字节长度，宿主按顺序搬字节，怎么切都行 |
| 宿主自己的 UDP 中继或 SFU | DATAGRAM，1200 |
| 一条宿主连接上跑多个会话 | 宿主给每条消息加自己的会话标识，按标识交给对应的 `rcs_session` |

- **两端由宿主配对**：分享端 `start` 后就在 `LISTENING`，等经通道到达的握手（多久都等，不开任何端口）；观看端 `start` 后立即经通道发出握手，`connect_timeout_ms`（默认 5 秒）内没有回应就失败。所以宿主应在分享端进入 `LISTENING` 之后再启动观看端，或者给观看端开着重连。
- **多个观看端**：和 UDP 上一样，每个观看端是一个自己的连接，最多 `max_viewers`（默认 64，最多 256）个。分享端第一次见到的 `route` 就是一个新观看端的握手（像 UDP 上一个新地址），同时最多 16 个握手，其余的等；加入以后它的 `rcs_viewer_info.route` 就是这个号。一个 `route` 同时只承载一个连接：观看端在同一个 `route` 上重新握手，旧的那个连接就结束；宿主把一个 `route` 给了另一个对端时，先对它调用 `rcs_session_transport_closed`。字节流按 `route` 各自分帧。为了让伪造很多 `route` 的洪水占不满分享端：一个还没握手完的 `route` 最多排 64 KiB、所有这样的合起来 1 MiB，同时最多 1024 个 `route` 在等握手，超过的 `rcs_session_transport_receive` 返回 `QUEUE_FULL`（数据报当作丢了；字节流丢了字节，那个 `route` 的连接结束、流重新开始）。
- **数据报**：每次 `on_transport_send` 是一条消息，原样作为一条消息送到对端；可以丢、乱序、重复。接收方总能收下 65535 字节以内的消息，所以两端的 `host_max_datagram` 可以不同。
- **通道换了新的**（同一个对端）：两端都调用 `rcs_session_transport_restarted`，再交新通道的字节。字节流：旧流上没收完的半个数据报丢掉，从新流的第一个字节起按帧读，丢了的由连接自己重发；数据报：只是告诉 SDK 路径变了（往返和码率重新估计）。都报告 `RCS_PATH_CHANGED`（cause `HOST`），同一个连接、同一个观看端编号，不再握手、不再准入。`rcs_paths_config.migrate` 为 0 时它按路径断开处理（`NETWORK`）。别的传输方式上是 `UNSUPPORTED`。
- **通道断了**（或通到了另一个对端）：`rcs_session_transport_closed(session, route)` 后，那条通道上的连接先读完关闭前已经交进来的数据（对端的告别可能就在里面，这时按它说的结束），然后按“路径断开”（`NETWORK`）结束：观看端按 `rcs_reconnect_config` 重连，重连的握手经宿主恢复后的通道发出；分享端上只有那个观看端离开，别的照旧，最后一个走了、开着 `keep_listening` 时回到 `LISTENING`。不调用时，`idle_timeout_ms`（默认 5 秒）没有声音算路径没了（`RCS_PATH_LOST`），连接按[网络变化](#网络变化连接迁移)留着，通道回来、数据接着到就继续。SDK 不会改走别的传输方式。
- 示例 `rcsdk_host_relay` 是一个按房间转发的小服务器（一个房间一个分享端、任意多个观看端，分享端那条连接上的每段字节前面带 8 字节的路由号），`examples/common/relay_channel.hpp` 是宿主一侧的写法，`rcsdk_cli --transport host --relay 地址:端口` 演示整个流程（`--viewers N` 在一个进程里起 N 个观看端）。

## 经转发器：`RCS_TRANSPORT_FORWARDER`

人多的时候（课堂、演示、几十人的群），分享端的上行撑不住“每人一份”。转发器是一台单独部署的服务器（`server/forwarder/`：程序 `rcs_forwarder`，或者库 `rcsfwd.h` 嵌进宿主自己的服务器，见 [22](22-examples-tools.md)），分享端把每张画面**只上传一次**，转发器给每个观看端各发一份：

- **画面**：分享端用一个“流密钥”把每张画面加密一次（每档编码各一份），转发器只看得到路由用的头（第几路、第几档、时域层、是不是关键帧），按每个观看端各自重传、加冗余、节流、做动态码率；某个观看端的路径远远跟不上（它那一跳量到的码率持续 1 秒不到这一档实际发出的一半、积压超过 1 秒、或者丢了积压）时，转发器当场把它移到正在跑的低一档，分享端随后报告 `RCS_EVENT_VIEWER_VIDEO`（原因 `PATH`）。上移和每档的码率仍由分享端按[群组策略](07-sharing.md#弱的观看端多一档编码rcs_ext_encodings)定，依据是转发器每 100 ms 报来的每个观看端的路径。
- **晚来的人**：转发器为每一档留着最新的关键帧和它之后的画面（默认 3 秒，密文原样），新来的人（和被移到另一档的人）直接从这里开始，分享端不用为它出关键帧；留着的太旧、太大（它的路径半秒装不下），或者它的流密钥打不开，转发器才向分享端要一个关键帧，同时等着的人合用一个。
- **声音**：分享端的声音按格式只编一次（通常大家是同一个格式，就只有一个编码器），每个包用流密钥加密一次、上传一次，转发器复制给要这个格式的观看端；声音的统计是会话的（`RCS_STATS_AUDIO_SEND`），不按观看端分。观看端自己的声音（麦克风）仍走端到端的连接。
- **分享端的上行**：分享端到转发器的那一跳有自己的动态码率，量出上行能装多少；所有路、所有档的编码合起来不超过它（减去声音），超出时按同样的比例压低，`RCS_EVENT_RATE` 的 `viewer_id` 为 0 说明是上行限住了，`rcs_forwarder_stats` 的 `uplink_bps`、`uplink_limited` 记着。上行丢了画面时，转发器让那一路的观看端从下一个关键帧重新开始（并替它们要一个），不会把坏掉的参考链转下去。
- **其余的一切**（握手、准入、控制、输入、数据通道、模块、观看端的声音、指针、流公告）是每个观看端和分享端之间**端到端**的连接，经转发器原样透传（“管道”）：转发器解不开、改不了，只能丢、拖、乱序，和 TURN 一样。这些功能经转发器时用法和直连一样，每个方向的量受转发器的 `max_pipe_bps` 限制（[24](24-forwarder.md#经转发器的各项功能)：测过什么、文件传得多快）。
- 两端都只向转发器发起连接，不开端口、不做 ICE、不需要信令。怎样连（`rcs_forwarder_config.link`，一次运行中不变）：`RCS_FORWARDER_UDP`（默认）和 `RCS_FORWARDER_TCP` 连 `connect.address` / `connect.port`（地址可以是名字，每次连接时解析，取第一个地址）；`RCS_FORWARDER_WEBSOCKET` 连 `websocket_url`：`ws://` 是转发器自己的 WebSocket 入口，`wss://` 是它前面的 TLS 代理（转发器不做 TLS，见 [24](24-forwarder.md#tls放在代理后面)），证书按系统的信任和 URL 的主机名核对，或者钉住 `tls_certificate_sha256`。`RCS_FORWARDER_HOST` 走宿主自己的通道（`RCS_EXT_HOST_TRANSPORT`，路由 0，`connect.address` 留空）：浏览器的观看端（页面的 WebSocket 连 WebSocket 入口，[23](23-browser.md#经转发器)），或只能经 HTTP 代理出去的网络（[24](24-forwarder.md#宿主自己的通道和浏览器)）。几种方式里那一跳完全一样（都端到端认证、加密），TCP 和 WebSocket 是给封 UDP、只放行 HTTPS 的网络的，底下 TCP 自己重传，丢一个包会卡住后面的。浏览器里只有 `HOST`（`rcs_capabilities.forwarder_links`），别的链路 `start` 时 `UNSUPPORTED`。

配置（两端）：

```cpp
rcsdk::SessionConfig config(RCS_ROLE_VIEW);
config.setKey(key)                                        // 端到端的秘密照旧：连接密钥、口令、PIN 或设备密钥
      .setForwarder("forwarder.example.com", 47995, forwarderSha256, token);   // UDP；RCS_FORWARDER_TCP 作第五个参数是 TCP
// 或者 .setForwarderWebSocket("wss://forwarder.example.com/rcs", forwarderSha256, token);
// = connect.transport FORWARDER，再加 RCS_EXT_FORWARDER：
//   forwarder_key_sha256  转发器公钥的 SHA-256（rcs_forwarder --identity-sha256），对不上就不发令牌
//   link、websocket_url、tls_certificate_sha256   怎样连（上面）；HOST 时再加 RCS_EXT_HOST_TRANSPORT
//   token                 宿主服务器给这一端签的令牌
//   refresh_token、user_data   每次重新连转发器之前问宿主要新令牌（可选）
//   key_rotation_ms       分享端：除了有人离开，流密钥还每隔这么久换一次（0：不定期换）
//   from_admission        分享端：1 让新人打不开它被放行之前封装的任何东西（不从转发器留着的开始，等下一个关键帧）
```

**令牌**由宿主的服务器签：和转发器共享一把密钥（可以同时有新旧两把，按编号区分），写明房间、角色（分享端发布、观看端订阅）、宿主自己的路由号、过期时间（Unix 秒）、可选的码率上限和受众（转发器身份的 SHA-256：共用一把密钥的另一台转发器不收）。格式固定（`rcsf1.` 开头，见 `rcsfwd.h` 的 `rcsfwd_sign_token` 和 [24](24-forwarder.md#令牌) 的二十行 Python），任何语言几行就能拼出来。令牌在过期前可以反复用（重连），转发器不记账。会话比令牌活得久时，`refresh_token` 回调在每次重新连转发器之前（观看端重连、常驻监听的分享端重新连上、转发器重启后）让宿主换一个（第一次连接不问），宿主也可以随时用 `rcs_session_update_config` 换。房间名和路由号转发器的运营方看得到：用不透明的值（哈希、随机号），不要放群号、用户号。

**谁能看**：令牌只让一端用转发器的带宽。观看端要看到画面，还要通过和分享端端到端的握手、分享端宿主的 `on_viewer_join`（可以 `RCS_ADMIT_LATER`）；流密钥只发给放行了的观看端。观看端的 `rcs_viewer_info.route` 是它令牌里的路由号，由转发器转述——转发器担保它，端到端没有证明它；要按人授权，用端到端的凭据（设备密钥、每人的连接密钥）。

**流密钥**：分享端每次运行随机生成，经各观看端自己的端到端连接发给它。每个关键帧（任何一路、任何一档）让密钥“往前推一步”（老成员自己推）；放行一个人时，它拿到的是转发器留着的那些关键帧所在的那一步（所以它能从那里开始，最多看到它来之前 `cache_ms` 的画面），留着的太旧时是再往前推一步的那把（看不到它来之前的任何画面）。不想让新人看到它来之前那几秒的分享端把 `from_admission` 设 1：新人总拿往前推一步的那把，从转发器向分享端要来的下一个关键帧开始（分享端为它多出一个关键帧，和直连时的新人一样）。有人离开（或者 `key_rotation_ms` 到了）时换一代新的随机密钥，发给留下的人，全部确认或 1 秒后改用新的，离开的人解不开之后的画面。观看端核对每张画面和每个声音包：解不开、头和内容对不上、重复的都丢掉并计数（`RCS_STATS_FORWARDER`：`media_rejected`、`media_replayed`、`media_unkeyed`），画面从下一个关键帧继续。

**失败**（都不自动换成直连或 P2P）：

| 情况 | 结果 |
| --- | --- |
| 令牌格式不对、签名对不上、密钥编号不认识、是给另一台转发器的、过期、角色不对；房间已经有分享端 | `RCS_ERROR_REFUSED`，原因是转发器给的 |
| 房间满了（`max_viewers_per_room`）；转发器的房间数（`max_rooms`）或端数（`max_ends`）满了 | `RCS_ERROR_BUSY` |
| 转发器出示的公钥不是 `forwarder_key_sha256`；`wss://` 的代理的证书不被信任、不是给 URL 的主机名的、不是钉住的那张 | `RCS_ERROR_AUTHENTICATION`，令牌没有发出去 |
| `websocket_url` 不对、`wss://` 的主机是地址却没钉住证书、`WEBSOCKET` 时填了 `connect.address`；`refresh_token` 写回的不是令牌 | 创建或更新时 `RCS_ERROR_INVALID_ARGUMENT`；回调写回的不对时这次运行以它失败 |
| 转发器不回答 | `RCS_ERROR_NETWORK`；常驻监听的分享端在 `reconnect_window_ms` 内每隔 `reconnect_interval_ms` 再连 |
| 房间里没有分享端 | 观看端像没有分享端在监听一样以 `NETWORK` 失败（原因写明），按重连设置重试 |
| 转发器停了、重启了，或关闭了房间 | 两端都按路径断开处理：观看端重连，常驻监听的分享端重新连上转发器 |

现在的范围：两端到转发器的那一跳经 UDP 时像 UDP 观看端的连接一样迁移、跟随 `rcs_paths_config` 的网络选择（管道跟着它走，分享端那一跳的 `RCS_EVENT_PATH` 的 `viewer_id` 为 0），只是不备用（`standby` 是 `UNSUPPORTED`）；经宿主的通道时：转发器看到的还是同一个对端（宿主的中继到它的 UDP 端口）时，宿主换了通道（`rcs_session_transport_restarted`）接着走；新连到它的 TCP、WebSocket 入口是新的对端，那一跳结束（宿主调 `rcs_session_transport_closed`），在宿主下一次打开的通道上重连；经 TCP、WebSocket 时不迁移，连接断了按断线处理。只在本机环回上测过（Mac；转发器和经它的会话场景也在 Ubuntu 虚拟机上，[24](24-forwarder.md#还没量的)）；浏览器经转发器只在 Mac 上一个内嵌的 Chromium 里本机环回跑过（[23](23-browser.md#经转发器)），正式版浏览器还没测。

## 网络变化：连接迁移

UDP、TCP、P2P、中继和 HOST 上，任一端的网络变了（走出 Wi-Fi 用上蜂窝、插上网线、VPN 开关、换了 Wi-Fi、NAT 换了映射、IPv6 临时地址轮换），连接都留着：同样的密钥、观看端编号、准入、控制、模块和在路上的消息，只换路径（QUIC 的连接迁移，用在 SDK 自己的协议上）。不重新握手、不再问 `on_viewer_join`、不从关键帧重来（丢了帧才恢复）。设置在 `rcs_paths_config`（`RCS_EXT_PATHS`，两端各管各的），不放进链时迁移开、所有网络都能用、不备用。

**怎么换**：

- **对端从新地址来**（它换了网络，或 NAT 换了映射）：连接先验证新地址（发一个只有对端答得出的挑战，答之前往那里发的不超过从那里收到的 3 倍），再把流量换过去；只有包号最新、不是探测的数据报才算。改过源地址的截获包答不出挑战，换不过去。
- **本端的网络变了**：系统报告（Apple 的 `nw_path_monitor`、Windows iphlpapi 的通知、Android 和其他系统每半秒看一次网卡；宿主自己的监视器可以用 `rcs_notify_network_change` 更早告诉 SDK）或发送出错时，当前路径要在一个往返加 300 ms 内回答挑战。没有绑定网卡的套接字跟着系统的默认网络走，对端看到的就是新地址，跟过来；不回答时，直连 UDP 的观看端换一个新套接字再试，P2P 和中继经宿主的信令重新打洞。
- **TCP**：观看端的 TCP 连接断了（对端或路上的 NAT 复位了它）、网络变了而路径不回答、路径沉默一秒（`path_stall_ms` 之后再等 1 秒）时，观看端在自己的线程上向分享端的地址再连一条 TCP（绑定在选定的网络上），交给连接：旧的那条已经断了时流量马上换过去，否则先用连接的密钥验证；分享端的端口按连接编号把新连接上来的数据交给原来的连接，它验证后跟过来（分享端的 `CHANGED` 原因是 `PEER`）。连不上时，路径回来之前每秒再连一次。不经宿主、不经信令。没确认的可靠消息在新连接上马上重发，按序、恰好一次送到；旧连接里没送到的画面分片要回或从关键帧恢复，声音丢了就丢了。分享端是被连的一方，不主动连观看端；观看端的 TCP 关了（`FIN`、`RST`）它马上知道，路径立即停滞。
- **重新打洞**（P2P、中继）：两端用新的候选重新做一次 ICE（P2P 只在直连候选之间，中继只经 TURN 服务器，迁移**不改变传输方式**，P2P 绝不改走中继），信令用连接的密钥封好、经 `on_signal` 走宿主的通道，找到的路径加进连接、验证后换过去（事件里 `signaled` 为 1）。两端同时发起时观看端的那份继续，分享端回答它。中继：TURN 服务器给了 RFC 8016 的移动票据时，分配直接挪到新套接字上，中继地址不变、对端看不到任何变化；没给时新分配一个，经信令。coturn 要 4.15 起、开 `mobility`（之前的版本有 CVE-2026-65981：带票据的刷新不核对是不是分配的主人），建议每个会话单独的 TURN 凭据。
- **路径沉默**：`path_stall_ms`（默认 1 秒）没收到任何东西（每 250 ms 一个 PING）就报告 `RCS_PATH_CHANGING`：停发视频、声音和数据（只发 PING、挑战和控制消息），分享端放开这个观看端按住的键、旧代次的输入作废；打开了备用路径时马上换过去。路径回来时报告 `CHANGED`（cause `STALLED`），接着走。
- **路径没了**：`idle_timeout_ms`（默认 5 秒）没收到任何东西，报告 `RCS_PATH_LOST`，连接留着：观看端进入 `RECONNECTING`（`NETWORK`），在重连时间窗（`reconnect_window_ms`，从断开算）内等路径回来，回来后回到 `RUNNING`——**同一个连接，没有新的 `PEER`、`STREAM` 事件，不要在 `RECONNECTING` 时清掉渲染器**；分享端留着这个观看端 `path_hold_ms`（默认 30 秒），期间不给它发画面（编码器不停），名额不够时新来的观看端被放行后，最早留着的那个让位（以 `NETWORK` 离开）。路径没了时观看端的控制权被收回（`RCS_EVENT_CONTROL`），回来后由宿主决定是否再给（`rcs_session_set_viewer_control`）；`restore_control = 1` 时回来自动恢复。
- **对端已经没有这个连接**（分享端重启过、留着的时间到了、宿主把观看端断开了）：分享端的 UDP 端口（TCP：观看端新连的那条 TCP 上）回一个“不认识这个连接”的数据报，P2P 和中继经信令回一份“没有这个连接”；观看端在路径沉默时收到它，马上照旧重连，不空等时间窗。
- 一端的 `migrate` 为 0 时两端都不迁移：对端的新地址算路径断了，路径没了 `idle_timeout_ms` 后连接结束；TCP 连接断了连接就马上以 `NETWORK` 结束，观看端照旧重连。还在协商、等准入的连接也是这样：观看端到 `RUNNING`、分享端到观看端加入之后，连接才迁移、才留着。

**宿主要做的**：

- 什么都不做也行：NAT 重新映射、双方可达时的换网络（局域网、公网地址）、TCP 连接断了、HOST 的数据报通道恢复，自动就好。
- P2P 和中继：会话期间保持信令通道，`on_signal` 随时可能给出新信令，`rcs_session_signal` 随时收（上面的[信令](#p2p-和中继信令)）。手机换网络时宿主到自己服务器的连接往往也断，等它恢复才能重新打洞（聊天类应用一般 1～3 秒）；备用路径不用信令。
- HOST：通道换了新的调 `rcs_session_transport_restarted`。
- Android：AAR 的清单带 `ACCESS_NETWORK_STATE`（安装时自动给）；交出 Java VM 后 SDK 才知道网络的种类和计费。宿主自己调了 `ConnectivityManager.bindProcessToNetwork` 时，之后的套接字固定在那个网络上，不跟着默认网络走。
- 直连 UDP 和 TCP 没有信令：分享端换到过滤型 NAT 后面时观看端找不到它，要穿 NAT 用 P2P。

**宿主看得到、管得了**：

```cpp
callbacks.onEvent = [&](const rcs_event& e) {
    if (e.type != RCS_EVENT_PATH) return;
    const auto& p = e.path;
    if (p.change == RCS_PATH_CHANGED && (p.to.network_flags & RCS_NETWORK_FLAG_METERED)) {
        ui.notice("已切换到按流量计费的网络");   // 换不换预设是宿主的事
    }
};
auto paths = rcsdk::defaults<rcs_paths_config>();
paths.excluded_networks = 1u << RCS_NETWORK_CELLULAR;   // 不用蜂窝
paths.policy = RCS_PATH_POLICY_STAY;                     // 网络来回时不跟着来回
session.update(paths);                                   // 运行中改立即生效
```

- `RCS_EVENT_PATH`：`CHANGING`、`CHANGED`（新旧路径、中断时长 `interruption_ns`、是否经过信令）、`LOST`、`STANDBY`，带原因（本端的网络、路径沉默、对端换了、NAT 重新映射、设置、HOST 通道）；分享端带 `viewer_id`。`rcs_path_info` 说路径走哪种路线（直连、经 NAT、中继、宿主通道）、本端走哪块网络（种类、是否计费、省流量、默认、VPN）、网卡名和两端地址。`rcs_session_get_path_info` 随时取当前路径和备用路径。网络统计有同样的事实，加上换路径的次数、最近一次的中断、经计费网络收发的字节（`metered_bytes`），见 [14](14-events-stats-logging.md)。
- `rcs_list_networks`：本机现在的网络（默认网络在前），`rcs_capabilities.network_facts` 说本系统能说出哪些事实（[19](19-platforms.md)）。
- `excluded_networks`：本端不用哪些网络，`(1u << rcs_network_kind)` 位；VPN 算它下面那块网络的种类。设了它（或 `policy = STAY`），本端的套接字绑定到一块允许的网络的网卡上（允许时是系统的默认网络），什么都不会经排除的网络发出；那块网络没了或被排除时换到另一块允许的，一块都没有时路径马上算没了（`LOST`，写明原因），连接照样留着。系统说不出网络种类时（没交出 Java VM 的 Android）是 `UNSUPPORTED`；分享端用 UDP、TCP 时它是被连的一方，也是 `UNSUPPORTED`；和 `local_interface` 一起设是 `INVALID_ARGUMENT`。
- `policy`：`FOLLOW_SYSTEM`（默认）跟着系统的默认网络走；`STAY` 绑在正在用的网络上，那块网络没了或被排除才换。
- `standby = 1`：在另一块允许的网络（以太网、Wi-Fi 或蜂窝，优先和正在用的不同种类）上一直备着一条验证过的路径，每 `standby_probe_ms`（默认 15 秒）一个几十字节的探测保持 NAT 映射；当前路径一沉默（`path_stall_ms`）就换过去，不等系统判定网络没了，原来的路径回答了再换回来。P2P 和中继的备用路径经信令建立，TCP 的是另一块网络上到分享端的第二条 TCP 连接（断了的话 `standby_probe_ms` 之后再连一条）。代价是探测的流量（计入 `metered_bytes`）和手机蜂窝无线电的耗电；只在系统让本进程在默认网络之外同时用那块网络时有用（Android 只让向 `ConnectivityManager.requestNetwork` 要过蜂窝的应用在 Wi-Fi 之外用它，SDK 不要，所以 Android 上多半建不起经蜂窝的备用路径，`RCS_PATH_STANDBY` 会说；没有实测）。VPN 是正在用的网络时不备用（不绕开隧道）。HOST 上、UDP 和 TCP 的分享端上是 `UNSUPPORTED`。

## Wi-Fi 周期性卡顿（AWDL）

**现象**：网线上很流畅，换到 Wi-Fi 后画面和声音**一起**卡，而且有节奏（不是随机的），大约每秒一次。

**原因**：多半是 Apple 设备上的 AWDL（Apple Wireless Direct Link）。它是隔空投送、隔空播放、接力、随航、通用控制共用的点对点 Wi-Fi，常驻在 2.4 GHz 的 6 信道、5 GHz 的 44 和 149 信道上。只要它开着，而 Mac（iPhone、iPad 也一样）连的网络不在这几个信道上，网卡大约每 1.05 秒（1024 个时间单位）就被它拉走几十毫秒。这段时间两个方向都什么也收不到、发不出，回来后积压的一次涌到。Moonlight、Parsec 这类串流程序在 Mac 的 Wi-Fi 上也一样。

实测（Mac 经 Wi-Fi 观看、PC 有线共享 4K）：

| 情况 | 结果 |
| --- | --- |
| Mac 在 2.4 GHz 的 2 信道 | 每 1.04～1.06 秒断 60～100 ms |
| 同一信道，`sudo ifconfig awdl0 down` | 不再卡 |
| 换到 5 GHz 的 44 信道（AWDL 开着） | 不再卡 |

**SDK 告诉宿主**（不改系统设置，宿主决定怎么处理）：

- **周期性断流检测**（`rcs_network_stats`，所有系统）：本端收到的路径上，30 ms 以上什么都没收到、而对端一直在发的断流。判断"对端一直在发"靠两样：之后到的数据报在路径上排了至少半个断流的时间，或者断流期间发的数据报丢了。对端自己没东西可发（静止画面）不算。字段：
  - `stalls`：最近 10 秒几次；
  - `stall_ms`：中位长度；`longest_stall_ms`：最长一次；
  - `stall_period_ms`：节奏稳定时的周期，否则为 0；
  - `peer_stall_period_ms`、`peer_stall_ms`：对端说它看到的周期和中位长度。
- **`RCS_EVENT_STALLS`**：节奏开始、变了一成以上、或消失时发出。`from_peer` 区分是本端看到的还是对端说的（分享端带 `viewer_id`）。事件里带本端当前的路径；路径走 Wi-Fi 时，还带那个 Wi-Fi 的信息。
- **`rcs_list_wifi`**：本机连着的 Wi-Fi 的频段、信道、带宽、信号、噪声、发送速率，以及 AWDL 是否开着。`other_channel` 为 1，表示 AWDL 开着、而网络在 2.4 或 5 GHz 的其他信道上，这时就会这样断流。各系统能说出哪些见 `rcs_capabilities.wifi_facts`（[19](19-platforms.md)）。6 GHz 网络上会不会这样还没测过，`other_channel` 为 0。

**宿主可以提示用户**（这些都是用户那边的设置，SDK 不会去改）：

1. 把路由器的 2.4 GHz 改到 6 信道，或者让设备连 5 GHz 的 44 或 149 信道。隔空投送等功能照常能用。
2. 用网线。
3. 临时关掉 AWDL：`sudo ifconfig awdl0 down`。代价是隔空投送、隔空播放、接力、随航、通用控制都停掉；有东西要用 AWDL 时 macOS 会自己把它重新打开，重启后也会恢复。

```cpp
callbacks.onEvent = [&](const rcs_event& e) {
    if (e.type != RCS_EVENT_STALLS || !e.stalls.period_ms) return;
    if (e.stalls.wifi.other_channel) {
        ui.notice("Wi-Fi 的信道和隔空投送（AWDL）冲突，每秒会卡一下：请改用 6、44 或 149 信道，或用网线");
    } else if (e.stalls.from_peer) {
        ui.notice("对方的网络每隔一段时间断一下");
    }
};
```

周期性断流也可能有别的来源：Windows 的后台扫描（`wifi_low_latency` 会关掉它），手机的省电。所以检测不限于 AWDL，`stall_period_ms` 只说节奏。本端的网络线程被卡住时，看起来也一样。

## 断线、常驻监听与自动重连

一次运行不因为连接断开而结束，除非宿主这样配置（`rcs_reconnect_config`）。连接结束的原因在状态事件的 `result` 里：

- `RCS_ERROR_DISCONNECTED`：对端关闭了连接（它调用了 `stop`，或进程正常结束）。
- `RCS_ERROR_RESTARTING`：对端关闭了连接，并说它会回来（`rcs_session_stop_with_reason(session, RCS_STOP_RESTART)`，C++ `session.stop(RCS_STOP_RESTART)`）：宿主程序更新后重启、助手要换到另一个登录会话时用。
- `RCS_ERROR_NETWORK`：路径出了问题。关掉了迁移时：`idle_timeout_ms` 没收到对方任何数据、套接字错误、TCP 连接断了。开着迁移时路径没了连接先留着（上面的[网络变化](#网络变化连接迁移)），观看端到重连时间窗用完、分享端到 `path_hold_ms` 用完才以它结束；对端说已经没有这个连接时观看端马上照旧重连。进程崩溃来不及通知，对端也是这样发现的。
- `RCS_ERROR_PROTOCOL`：对端违反了协议（观看端不对它重连）。

**分享端**（`keep_listening`，默认 1）：观看端离开后回到 `LISTENING`，对端信息和流信息清空，按住的键释放；下一个观看端从头协商、公告画面、授权。P2P 和中继不需要宿主做什么：分享端一直回答新来的观看端信令。设 `keep_listening = 0` 则观看端一走运行就以这个原因结束（`FAILED`）。观看端或路径造成的失败（连通检查失败、没有共同格式、违反协议、握手失败）也回到 `LISTENING`；分享端自己的失败才结束运行。

**观看端**（`reconnect`，默认 1）：运行中路径没了（`NETWORK`）先在留着的连接上等路径回来（`RECONNECTING`，回来后 `RUNNING`，同一个连接）；连接结束了（对端说已经没有这个连接、关掉了迁移）或分享端说会回来（`RESTARTING`），每 `reconnect_interval_ms`（默认 1 秒；`RESTARTING` 和“没有这个连接”时立即）连一次（P2P 和中继给一份新的信令），直到连上或 `reconnect_window_ms`（默认 30 秒，从路径断开算，0 为一直试到 `stop`）用完，这两个都在 `rcs_network_tuning_config`；中间不再报告 `CONNECTING`。新连接连上后状态回到 `RUNNING`，画面从关键帧开始。时间窗用完以 `RCS_ERROR_NETWORK` 结束。分享端正常关闭了连接时不重连；第一次就连不上也不重试（地址不对、对方没开是宿主该处理的失败）；重连时遇到 `BUSY`（分享端可能还占着刚断开的那个连接）在时间窗内继续重试。重连期间 `send_input`、`request_keyframe` 返回 `INVALID_STATE`，`rcs_session_update` 照常。

局域网上两端各在 5 秒内发现断开，新连接通常在断开后 6～7 秒内建立。每次通过握手的连接计入网络统计的 `connections`，迁移不计。

```cpp
#include <rcsdk/tuning.hpp>

rcsdk::SessionConfig view(RCS_ROLE_VIEW);
auto& network = view.extension<rcs_network_tuning_config>();   // 从预设的值开始
network.reconnect_window_ms = 120'000;   // 断线后重连 2 分钟
network.reconnect_interval_ms = 2'000;   // 每 2 秒一次
callbacks.onEvent = [&](const rcs_event& e) {
    if (e.type == RCS_EVENT_STATE && e.state.value == RCS_STATE_RECONNECTING) {
        ui.showReconnecting(rcs_result_message(e.state.result));
    }
};
// 一次性共享：对方一走就结束
rcsdk::SessionConfig share(RCS_ROLE_SHARE);
share.setKeepListening(false);
```

## 数据通道

两端的宿主可以通过会话的加密连接收发自己的消息：`rcs_session_send_data` 发，对端的 `on_data` 收。SDK 不解释内容，`channel` 是宿主自己编的号（例如 1 标注、2 命令）。更复杂的功能（两端互报、按观看端收发、在被控的机器上跑一半）做成模块（[15](15-modules.md)）；文件传输用官方模块。

- **可靠消息**（默认）：整条、按发送顺序到达（跨通道也有序），一条最大 16 MiB（`RCS_DATA_MAX_MESSAGE`）。
- **不可靠消息**（`RCS_DATA_UNRELIABLE`）：不重传，可能丢、可能乱序，一条最大 1024 字节（`RCS_DATA_MAX_DATAGRAM`）；TCP 上只在路径远慢于发的量、本端的队列装不下时丢。适合下一条会覆盖上一条的东西（拖动中的标注位置）。
- **优先级**：输入不排在数据后面；视频有分片待发时数据不发，数据还有自己的拥塞窗口：路径一出现排队或丢包就先退让，所以数据用的是视频剩下的带宽，不压低视频的码率。在路上、还没确认的不超过 `in_flight_bytes`（默认 64 KiB）。
- **流量控制**：`rcs_session_send_data` 不阻塞，消息被复制进队列；队列（`send_queue_bytes`，默认 64 MiB）装不下时返回 `QUEUE_FULL`，之后队列又有空间时发 `RCS_EVENT_DATA`（`queued_bytes`）。发大量数据的正确写法：发到被拒绝为止，等事件，再发。
- **生命周期**：只能在 `RUNNING` 时发（重连期间 `INVALID_STATE`）；换路径不影响已经发出的：在路上的照常确认和重发，恰好一次、按序到达；连接结束时还没发出的消息丢弃，重连后由宿主重发。分享端每个观看端一个队列：`viewer_id` 0 发给所有观看端（要么都进、要么都不进），编号发给那一个；`on_data` 的 `viewer_id` 是发送者。
- **`on_data`** 在读连接的线程上，画面和输入排在它后面：复制需要的内容后尽快返回。
- 统计分区 `RCS_STATS_DATA`。

```cpp
callbacks.onData = [&](const rcs_data_message& m) {
    inbox.push(m.channel, std::vector<uint8_t>(m.data, m.data + m.size));   // 复制，交给自己的线程
};
if (session.sendData(2, "refresh") == RCS_ERROR_QUEUE_FULL) {
    // 等 RCS_EVENT_DATA 再发
}
```
