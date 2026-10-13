# 10 身份、密钥与安全

握手时两端互相证明身份。SDK 只做证明和核对；密钥和口令保存在哪里、哪些设备可信、怎样配对、怎样给用户看，都由宿主决定，SDK 一个字节都不落盘。

## 连接是怎样保护的

- 每次握手两端各生成一次性的 P-256 密钥对（ECDH），会话密钥由 HKDF 从 ECDH 结果和两端共有的秘密派生：没有共享秘密（或没有受信任的设备私钥）的人完成不了握手，也换不了中间的公钥；会话结束后一次性私钥丢弃，共享秘密日后泄露也解不开录下的流量（前向保密）。
- 记录和数据报用 AES-256-CTR 加密、HMAC-SHA256（截断到 128 位）认证，两个方向用不同的密钥，重放、乱序、篡改都被拒绝。所有传输（UDP、TCP、P2P、中继、宿主的通道）上都一样：中继、宿主的服务器、运营商只看得到密文。
- 密码学用系统的（macOS、iOS 的 Security / CommonCrypto，Windows 的 CNG），没有系统密码学的地方（Android）用编进 SDK 的 BearSSL；短 PIN 的 PAKE 用 Monocypher 的 X25519。
- 握手失败的连接只影响它自己：分享端丢弃它（计入 `rejected_connections`）、继续监听，端口扫描或错误的密钥结束不了分享会话。
- 正式上线前仍建议做一次独立的安全评估（SDK 还没做过）。

## 两端共有的秘密：`rcs_session_config.identity`

`identity.secret` 两端必须一样（不一样时失败原因会说明双方各用什么）：

| `secret` | 适合 | 说明 |
| --- | --- | --- |
| `RCS_SECRET_KEY`（默认） | 宿主有自己的通道交换密钥 | `identity.key`，32 字节随机数（`rcs_generate_connection_key`）。不是口令 |
| `RCS_SECRET_PASSWORD` | “设备号 + 口令”，用户手动输入 | 两端用 PBKDF2-SHA256（60 万轮，盐由分享端每次运行随机选）把 `identity.password` 变成密钥。**不是 PAKE**：录下一次握手、或者自己连上分享端的人，都可以离线一个个猜，每猜一次要算 60 万轮。所以口令必须是长随机串，10 位以上随机字符（约 50 bit），由宿主生成显示给用户；6 位数字几分钟内就会被猜中 |
| `RCS_SECRET_PIN` | “设备号 + 短 PIN”：一端显示、对面的人照着输入，例如远程协助时被协助的人念出 6 位数字 | PIN 放在 `identity.password` 里，4～32 个字符（通常是数字，`rcs_generate_pin`）。握手里用 PAKE（CPace on X25519）证明：录下握手的人一次也猜不了，每次握手只能让对方在线猜一次。分享端数失败的次数，到 `identity.pin_attempts`（默认 5，1..1000）次后不再接受 PIN 握手，直到宿主重新设置身份；6 位数字、5 次，猜中的机会是二十万分之一。观看端每次连接只验证一个答复：冒充分享端（或在路径上）的人只能猜一次，猜错观看端就以 `RCS_ERROR_AUTHENTICATION` 结束这次运行（不重连）；这样伪造的答复也会让这次连接失败，和 PIN 错一样。UDP 上分享端只回答证明了自己地址的 PIN 问候（多一个往返），伪造源地址的问候占不了次数。每次会话一个新 PIN |
| `RCS_SECRET_NONE` | 已配对的设备之间 | 不共享任何秘密，只靠设备密钥：两端都必须有 `RCS_EXT_DEVICE_IDENTITY` 的 `device_key` 和 `trusted_keys`，否则创建会话返回 `INVALID_ARGUMENT` |

运行中换身份用 `rcs_session_set_identity`（核心配置的字段不能经 `rcs_session_update` 改），从下一个握手起生效（分享端监听着的下一个观看端、观看端重连），已经连着的不受影响；它也让 PIN 的失败计数从零开始。

## 设备密钥：`rcs_device_identity_config`

一个长期的 P-256 密钥对，证明“我是哪台设备”：

- 用 `rcs_generate_device_key` 生成（`RCS_DEVICE_KEY_SIZE` = 97 字节：公钥 + 私钥，SEC 1），宿主自己保存（见下文“密钥存在哪里”）；公钥是前 `RCS_PUBLIC_KEY_SIZE` = 65 字节，`rcs_device_public_key` 也能取出并顺便校验。
- 有设备密钥（`has_device_key = 1`）的一端在每次握手里签名；对端验证签名后得到它的公钥（`rcs_peer_info.public_key`，`RCS_EVENT_PEER` 和 `on_viewer_join` 的 `viewer->peer` 里也有）。公钥在握手里加密传送，被动监听的人看不到。
- `trusted_keys`（`trusted_key_count` 个公钥，最多 1024）：只接受证明了其中某个公钥的对端；为空时不限制，只报告对端出示了什么。
- 分享端不信任观看端时：告诉它被拒绝了、丢弃这个连接、继续监听；观看端以 `RCS_ERROR_AUTHENTICATION` 失败，原因以 “The sharer refused this end” 开头，不重连（宿主可以提示“这台设备还没有被对方授权”）。
- 观看端不信任分享端时：本次运行以 `RCS_ERROR_AUTHENTICATION` 失败，`rcs_peer_info.public_key` 里是分享端实际出示的公钥（宿主可以提示“对方设备变了”或让用户决定是否信任）。
- 运行中用 `rcs_session_update` 换设备密钥和信任列表，从下一个握手起生效。

**常见的组合**：

- **首次配对后免密码**：第一次用口令或 PIN 连接（`trusted_keys` 为空，两端都带 `device_key`），连上后各自从 `rcs_peer_info` 记下对方的公钥；以后改用 `RCS_SECRET_NONE` + 对方的公钥。
- **宿主有服务器**：服务器分发各设备的公钥（服务器只经手公钥，即使被攻破也冒充不了任何一端），两端用 `RCS_SECRET_NONE`。前提是信任列表来自可信的配对。
- **人工核对**：把公钥的哈希截一段显示给两边的用户比对（格式由宿主定）。

```cpp
// 被控端：口令由宿主随机生成、显示在界面上
auto share = rcsdk::SessionConfig(RCS_ROLE_SHARE).setAddress("0.0.0.0", 47990).setPassword(randomCode);
// 远程协助：被控端显示 6 位 PIN，观看端的用户照着输入
const auto pin = rcsdk::generatePin(6);
auto helped = rcsdk::SessionConfig(RCS_ROLE_SHARE).setAddress("0.0.0.0", 47990).setPin(pin);   // 默认失败 5 次后锁住
auto helper = rcsdk::SessionConfig(RCS_ROLE_VIEW).setAddress(ip, 47990).setPin(typedPin);
// 锁住以后（RCS_EVENT_PIN 的 attempts_left 为 0）换一个新 PIN，计数从零开始
rcsdk::Session session(helped);
const auto next = rcsdk::generatePin(6);
rcs_identity identity = helped.core().identity;
std::memcpy(identity.password, next.c_str(), next.size() + 1);
session.setIdentity(identity);
// 已配对的设备：不共享秘密，各自信任对方的公钥
const rcsdk::DeviceKey mine = loadFromKeychain();   // 首次用 rcsdk::generateDeviceKey()
auto view = rcsdk::SessionConfig(RCS_ROLE_VIEW)
                .setAddress(ip, 47990)
                .setSecret(RCS_SECRET_NONE)
                .setDeviceKey(mine)
                .setTrustedKeys({pairedSharerPublicKey});   // rcsdk::PublicKey
```

## 短 PIN 的典型流程

1. 分享端宿主用 `rcs_generate_pin`（C++ `rcsdk::generatePin(6)`）生成 6 位数字，显示在界面上，用它启动会话。
2. 用户把 PIN 念给或发给对面的人；观看端宿主让他输入，同样设成 PIN 后连接。
3. PIN 对：照常连上。PIN 错：观看端以 `RCS_ERROR_AUTHENTICATION` 失败，原因是 “Wrong PIN or another secret”，宿主让用户重新输入（新的一次运行）；分享端继续监听，收到 `RCS_EVENT_PIN`（`rcs_pin_event`：失败几次、还剩几次），`rcs_network_stats.pin_failures` 加一。UDP 上分享端要等这次尝试结束（回答后 `connect_timeout_ms` 没等到观看端的证明，默认 5 秒）才知道失败了。
4. 失败到 `pin_attempts` 次后，分享端回答 PIN 握手的只是一条短通知，观看端以 `RCS_ERROR_AUTHENTICATION` 失败，原因是 “The sharer accepts no more PIN attempts”。宿主换一个新 PIN 并 `rcs_session_set_identity`（同一个 PIN 也行），计数从零开始；新的一次运行也从零开始。正在进行的握手占着一次尝试，所以同时猜也不会超过次数。分享端回答了的握手才可能算失败（回答本身就让对方能验证一次猜测，所以回答后对方不再出声也算一次）；回答之前就被拒绝的问候（格式不对、CPace 份额是低阶点）不占次数。能连到分享端的人用自己的猜测把次数用完，正确的 PIN 也会被挡住，直到宿主重新设置身份：这是次数上限本身的代价，`RCS_EVENT_PIN` 让宿主及时知道。

这条通知没有认证：路径上能伪造它的人只能让观看端放弃（丢掉它的包也能做到）。PIN 只用来连上这一次；要长期免输入，连上后按上面“首次配对后免密码”记下对方的设备公钥。

## 密钥存在哪里

SDK 不保存任何密钥、口令或信任列表。建议：

| 系统 | 设备私钥、连接密钥、口令 |
| --- | --- |
| macOS、iOS | 钥匙串（Keychain）；iOS 的广播扩展和应用之间用共用的钥匙串访问组或 App Group |
| Windows | 用户的数据用 DPAPI（`CryptProtectData`）或凭据管理器；服务（LocalSystem）的用机器范围的 DPAPI，或只有 SYSTEM 和管理员能读、设了 ACL 的文件 / 注册表项（[16](16-unattended.md)） |
| Android | 用 Android Keystore 的密钥加密后存在应用的私有目录里 |

- 公钥不是秘密，可以放在服务器上分发。
- 宿主的服务器能读到它转发的共享密钥时，它也就能冒充任一端：这与宿主对自己服务器的信任一致。不想信任服务器就用设备密钥（`RCS_SECRET_NONE`）。
- TURN 凭据建议由宿主后端按会话签发、有时效（coturn 的 `use-auth-secret`）。
- 信令文本里没有密钥，但有本机的地址，按隐私数据转发（[09](09-transports.md#信令文本的格式)）。

## 宿主还要负责的

- **授权和提示**：远程控制默认关闭（`rcs_control_config.allowed = 0`）。被控的机器上应该显示有人在看、有人在控制，并能随时收回（`rcs_session_update` 一个 `allowed = 0` 的 `RCS_EXT_CONTROL`、`rcs_session_disconnect_viewer`、`rcs_session_stop`）。每次授权变化 SDK 都换一个“授权代次”，旧代次下产生的输入一律丢弃；撤销、断线、停止时释放所有按住的键和按钮。`on_input` 能逐条否决输入（[11](11-input.md)）。
- **准入**：远程协助常见的“某某请求连接，允许 / 拒绝”用 `RCS_ADMIT_LATER`（[07](07-sharing.md#准入先问用户再放行)）。
- **录制要不要告诉对方**是宿主的产品决定：SDK 不在画面上加标记，也不通知另一端（[17](17-replacing.md#编码包交给宿主录制转发审计)）。
- **无人值守**时机器前没有人点“允许”，访问控制全靠身份方式：用设备密钥（`RCS_SECRET_NONE` + 信任列表）或长随机口令，不要用 PIN（[16](16-unattended.md)）。
- **剪贴板**默认关闭（可能有密码），由宿主打开；被标为密码的复制默认不发（[15](15-modules.md)）。
- SDK 没有任何隐藏运行的支持：不隐藏进程、不隐藏窗口、不规避检测。
