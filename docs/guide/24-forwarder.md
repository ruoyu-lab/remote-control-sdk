# 24 部署转发器（Linux、macOS、Windows）

转发器（`server/forwarder/`）是给人多的群用的服务器：分享端把每张画面和声音只上传一次，转发器替每个观看端重传、节流、按路径换档，晚来的人从它留着的关键帧开始。两端怎样用见 [09](09-transports.md#经转发器rcs_transport_forwarder)，这一页讲宿主怎样把它跑起来：构建、身份、配置、令牌、TLS、作为服务运行、看什么。

它有两种形态，宿主选一种：

- **程序 `rcs_forwarder`**：配置文件、每个事件一行日志；Linux 上作为 systemd 服务或在容器里，macOS 上作为 launchd 任务，Windows 上作为服务。
- **库 `rcsfwd.h`**（`RemoteControl::Forwarder`）：嵌进宿主自己的服务器，C 接口，令牌可以由宿主自己核对（`verify_token`），统计和事件直接拿。程序就是这个库加一个配置文件的读法。

转发器是服务器，**Linux 是它的首要平台**：Ubuntu 24.04 LTS、26.04 LTS，x86-64 和 arm64，服务器版即可（SDK 的分享端和观看、控制端不支持 Linux，转发器支持）。macOS、Windows 上也能跑，例如局域网里的一台桌面机。

## 构建和安装

Linux（Ubuntu；GCC 13 以上或 Clang 18 以上、CMake 3.25 以上）：

```sh
sudo apt install cmake ninja-build g++
cmake --preset linux-forwarder -DBUILD_TESTING=OFF
cmake --build build/linux-forwarder --target rcs_forwarder
sudo cmake --install build/linux-forwarder --component forwarder --prefix /usr/local
```

预设 `linux-forwarder` 只编转发器用到的：SDK 的可移植部分和编进去的 BearSSL（不用系统的 OpenSSL），不编编解码库、示例和工具。装出 `bin/rcs_forwarder`、`lib/librcsfwd.so*`（程序按 `$ORIGIN/../lib` 找它，不用 `ldconfig`）、`include/rcsfwd.h`、CMake 包（`RemoteControlForwarderConfig.cmake`）和 `share/rcs-forwarder/`：`forwarder.conf.example`（每一项都有）、`rcs-forwarder.service`（systemd）、`rcs-forwarder.logrotate`、`rcs-forwarder.plist`（launchd）、许可证（`LICENSE`、`THIRD_PARTY_NOTICES.md`，二进制里有 BearSSL）。先跑它的测试：不加 `-DBUILD_TESTING=OFF`，`cmake --build --preset linux-forwarder && ctest --preset linux-forwarder`；预设 `linux-forwarder-scenarios` 再加上 `wss://` 和经转发器的会话场景（要 OpenH264，x86-64 上 `sudo apt install nasm`），见 [BUILDING](../BUILDING.md)。

macOS、Windows：`RCSDK_BUILD_FORWARDER` 在顶层工程里默认 ON，`release`（Windows：`windows-msvc`，或在 Mac 上交叉编译的 `windows-x64-release`）编出它，`cmake --install <构建目录> --component forwarder --prefix <目录>` 同样装出程序、库和样例。

## 身份和密钥

```sh
rcs_forwarder --generate-identity identity.key    # 写出私钥（十六进制），打印公钥的 SHA-256
rcs_forwarder --identity-sha256 identity.key      # 以后再看这个 SHA-256
openssl rand -hex 32                              # 一把令牌密钥（Windows：见下面的 PowerShell）
```

- **身份**：转发器在每次握手里证明自己持有这把私钥。打印出来的 SHA-256 有两个用处：两端的 `rcs_forwarder_config.forwarder_key_sha256`（对不上就以 `AUTHENTICATION` 失败，令牌不发出去），以及令牌的受众（下面）。私钥文件只让转发器的账户读。
- **令牌密钥**：宿主的服务器用它给每一端签令牌，转发器用它核对，32 字节，带一个编号；最多同时 4 把（换密钥时新旧并存，按编号区分）。Windows 上生成：`$b = New-Object byte[] 32; [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($b); ($b | ForEach-Object { $_.ToString('x2') }) -join ''`。

## 配置文件

一行一项 `名字 = 值`，`#` 之后是注释，没写的和写 0 的取默认值。每一项就是 `rcsfwd_config` 的一个字段（头文件里有完整说明）：

| 项 | 默认 | 说明 |
| --- | --- | --- |
| `listen_udp`、`listen_tcp`、`listen_websocket` | 不听 | `地址:端口`，数字地址（IPv6 写成 `[::]:端口`），至少一个。UDP 给能用 UDP 的端；TCP 给封 UDP 的网络；WebSocket（`ws://`，任何路径）给只放行 HTTP(S) 的网络和浏览器，`wss://` 由前面的 TLS 代理做（下面），这时它只听环回 |
| `identity_key_file` | 必填 | 身份私钥文件（194 个十六进制数字） |
| `secret` | 必填（1～4 行） | `编号:64 个十六进制数字` |
| `max_rooms` | 64 | 同时的房间数，1..65 536；满了新房间的端以 `BUSY` 被拒 |
| `max_viewers_per_room` | 64 | 1..256；满了以 `BUSY` 被拒 |
| `max_ends` | 1024 | 所有入口加起来同时连着的端（每个端一个读线程和它那一跳的线程）；TCP 和 WebSocket 的连接从接受时就算，超出的连接立刻关掉 |
| `max_viewer_bitrate_bps` | 100 Mbit/s | 发给一个观看端的上限；令牌里的码率上限在它之内 |
| `max_pipe_bps` | 50 Mbit/s | 一个观看端和分享端之间端到端连接（控制、输入、指针、数据通道、剪贴板、文件传输、观看端的声音）每个方向的上限，超出的丢掉并计数；经转发器的文件传输和大的剪贴板内容最快就是这么快（[下面](#经转发器的各项功能)） |
| `max_publish_bitrate_bps` | 400 Mbit/s | 一个分享端的画面和声音进它房间的上限，超出的丢掉并计数（观看端按丢包恢复） |
| `cache_ms` | 3000 | 每一档最新的关键帧和它之后的画面留多久，给晚来的人（100..60000）。新人因此能打开它被放行之前至多这么久的画面；不想这样的分享端设 `rcs_forwarder_config.from_admission`（[09](09-transports.md#经转发器rcs_transport_forwarder)） |
| `cache_bytes` | 16 MiB | 一个房间所有档留的合计；超出时那一档留的清掉，等下一个关键帧 |
| `max_cache_bytes` | 256 MiB | 所有房间留的合计，同样 |
| `log_file` | 标准输出 | 事件行追加到这个文件。systemd 服务的标准输出就是 journal，不写也行；写了就放在 `/var/log/rcs-forwarder/`，收到 SIGHUP（`systemctl reload`）时重新打开（logrotate 挪走它之后）。Windows 服务没有控制台，要写它 |

事件行的样子：`2026-10-12 03:19:09 forwarder: joined, room "cli", viewer, peer 1, route 7, WebSocket 127.0.0.1:58728`（房间开、关，端进、出、被拒和原因）。

## 入口和防火墙

- 防火墙放行转发器听的端口（入站）：UDP 一个、TCP 一个；WebSocket 放在代理后面时只放行代理的 443。是否放行、放行谁是管理员的决定，SDK 不改防火墙。Ubuntu 的 ufw：`sudo ufw allow 47995/udp`、`sudo ufw allow 47995/tcp`、`sudo ufw allow 443/tcp`；云服务器还有安全组。
- 三种入口上那一跳完全一样（同样的握手、加密、重传、节流、动态码率），只是 TCP 上每个数据报前面加两字节长度，WebSocket 上每个数据报是一条二进制消息。TCP 底下自己重传，丢一个包会卡住后面的，所以 UDP 能用时用 UDP。
- 每个入口自己限着：WebSocket 的开场请求 5 秒内、8 KiB 以内到齐，不是 WebSocket 开场的请求得到写明原因的 HTTP 回答（400、405、426、431）后关掉；违反 RFC 6455 的帧（没掩码、保留位、未知操作码、长度不是最短写法、超过 65 535 字节、拆开的控制帧、文本消息）得到写明原因的关闭码后关掉；10 秒内没完成那一跳握手的连接关掉；每条连接等着发出的数据最多 256 KiB，再多的按满了的 UDP 套接字一样丢掉，交给那一跳的重传和码率控制。

## TLS：放在代理后面

转发器自己不做 TLS，`wss://` 由前面的反向代理终止，再把 WebSocket 连接交给 `listen_websocket`。这样定的理由：

- 那一跳本来就端到端认证、加密（两端按 SHA-256 核对转发器的身份，媒体还有流密钥），TLS 在这里只为穿过只放行 HTTPS 的网络，不多保护什么。
- 穿网络要的是浏览器和系统都信的证书：公共 CA、按时续期、按域名签。反向代理（Caddy 自动申请，nginx 配 certbot，Windows 上 IIS）把这件事做得很成熟；转发器自己做就要在三个系统上各写一个 TLS 服务端（Network.framework、Schannel、BearSSL 只有 TLS 1.2），再自己管证书，多出一大块攻击面。
- 公司网络里做 TLS 检查的代理会换掉证书：系统信任它的根证书时，`wss://` 照样能通（按系统信任核对），钉住证书（`tls_certificate_sha256`）的反而不通。

nginx（`location` 里转发 WebSocket 升级）：

```nginx
server {
    listen 443 ssl;
    server_name forwarder.example.com;
    ssl_certificate     /etc/ssl/forwarder.example.com.pem;
    ssl_certificate_key /etc/ssl/forwarder.example.com.key;
    location /rcs {
        proxy_pass http://127.0.0.1:47996;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_read_timeout 1h;   # 那一跳每 250 ms 有数据，这只防空闲的连接
        proxy_buffering off;
    }
}
```

Caddy：`forwarder.example.com { reverse_proxy /rcs 127.0.0.1:47996 }`（Caddy 自动申请证书、自动转发 WebSocket）。IIS：装“WebSocket 协议”功能和 Application Request Routing，反向代理规则指向 `http://127.0.0.1:47996/rcs`。

Linux 上代理和转发器放在同一台机器，`listen_websocket = 127.0.0.1:47996`，只有代理的 443 对外。Ubuntu：`sudo apt install nginx certbot python3-certbot-nginx`，上面的 `server` 块放进 `/etc/nginx/sites-available/rcs-forwarder`、链接到 `sites-enabled/`，`sudo certbot --nginx -d forwarder.example.com` 申请证书并写好 `ssl_certificate`（`/etc/letsencrypt/live/<域名>/`）、按时续期，`sudo nginx -t && sudo systemctl reload nginx`。Caddy（按它的说明装官方的 apt 源）：那一行写进 `/etc/caddy/Caddyfile`，`sudo systemctl reload caddy`。

两端：`rcs_forwarder_config.link = RCS_FORWARDER_WEBSOCKET`、`websocket_url = "wss://forwarder.example.com/rcs"`（C++：`setForwarderWebSocket(url, sha256, token)`），`connect.address` 留空。证书按系统的信任和 URL 里的主机名核对；代理用自签名证书时把证书（DER）的 SHA-256 填进 `tls_certificate_sha256`。端只连 URL 里的主机，不用系统的 HTTP 代理设置：只能经 HTTP 代理（CONNECT）出去的网络里，宿主自己打通到转发器的连接，交给 `RCS_FORWARDER_HOST`（下一节）。

## 宿主自己的通道和浏览器

`rcs_forwarder_config.link = RCS_FORWARDER_HOST`：那一跳不由 SDK 自己连，而走宿主给的通道（`RCS_EXT_HOST_TRANSPORT`，路由 0；C++：`setForwarderHost(sha256, token)` 加 `rcsdk::hostTransport(...)`），`connect.address` 留空。`DATAGRAM` 的通道每条消息一个数据报，接到 WebSocket 入口（或 UDP 端口）；`STREAM` 的通道接到 TCP 入口（两字节长度的分帧就是宿主通道的分帧），例如宿主经 HTTP 代理的 `CONNECT` 打通的连接。那一跳和别的链路完全一样（核对身份、令牌、重传、码率），转发器分不出来。一条连接是一个那一跳：断了调 `rcs_session_transport_closed`，端按重连设置在宿主下一次打开的通道上重新连。

浏览器观看端就是这样连的（[23](23-browser.md#经转发器)）：页面的 WebSocket 连 `wss://`（代理）或 `ws://`（本机试验）上的 WebSocket 入口，令牌由页面向宿主的服务器要。部署时：

- 证书要浏览器信任（公网 CA），`tls_certificate_sha256` 在浏览器里用不了。
- 转发器不看 `Origin`：令牌、转发器的身份、端到端的握手才是凭据，没有 Cookie 那样随请求自动带上的东西可以被别的网站借用。宿主想只让自己的网站来连，可以在代理上按 `Origin` 拦。
- 页面的 CSP `connect-src` 写代理的 `wss://` 地址。

## 令牌

宿主的服务器给每一端签一个令牌，写明房间、角色（分享端发布、观看端订阅）、宿主自己的路由号（分享端的 `rcs_viewer_info.route`）、过期时间、可选的码率上限和受众（转发器身份的 SHA-256：签给这台转发器的令牌，另一台共用同一把密钥的转发器不收）。格式（`rcsf1.`，大端）：

| 字段 | 字节 |
| --- | --- |
| 版本 1 | 1 |
| 密钥编号 | 4 |
| 角色（1 发布，2 订阅） | 1 |
| 路由号 | 8 |
| 过期时间（Unix 秒） | 8 |
| 码率上限（0：不限） | 4 |
| 受众（转发器身份的 SHA-256） | 32 |
| 随机数 | 16 |
| 房间名长度和字节 | 1 + 1..64 |

令牌是 `rcsf1.` + base64url（不补 `=`）的上面这些字节 + `.` + base64url 的 HMAC-SHA256（密钥是那把令牌密钥，内容是 `rcsf1.` 加上面这些字节）。试用时用程序签：

```sh
rcs_forwarder --sign-token --secret 1:<64 位十六进制> --room <房间> --role publish|subscribe \
              --identity identity.key [--route N] [--expires-in 秒] [--max-bitrate BPS]
# 没有私钥文件的机器上：--audience <转发器身份的 SHA-256> 代替 --identity
```

宿主的服务器（例如 Python，二十行）：

```python
import base64, hashlib, hmac, os, struct, time

def b64(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()

def sign_token(key_id, secret, room, role, route, audience_hex, valid_for=600, max_bitrate=0):
    """role: 1 发布（分享端），2 订阅（观看端）；secret: 32 字节；audience_hex: 转发器身份的 SHA-256。"""
    room = room.encode()
    payload = struct.pack(">BIBQQI", 1, key_id, role, route, int(time.time()) + valid_for, max_bitrate)
    payload += bytes.fromhex(audience_hex) + os.urandom(16) + bytes([len(room)]) + room
    mac = hmac.new(secret, b"rcsf1." + payload, hashlib.sha256).digest()
    return "rcsf1." + b64(payload) + "." + b64(mac)
```

（用程序签的令牌核对过这段：同样的密钥算出同样的签名。）

建议：

- 令牌短一点（几分钟到一小时），每台转发器自己一把令牌密钥。令牌过期前可以反复用（重连要用），转发器不记账。
- 运行得比令牌久的会话要换令牌：两端的 `rcs_forwarder_config.refresh_token` 回调在每次重新连转发器之前（观看端重连、常驻监听的分享端重新连上、转发器重启后）被问一次，宿主在里面向自己的服务器要一个新的写回去（可以花点时间，这次连接等它）；也可以随时用 `rcs_session_update_config` 换，从下一次连接起用。
- 房间名、路由号用不透明的值（哈希、随机号），转发器的运营方看得到它们。

## 作为服务运行

### Linux：systemd

装好之后（上面“构建和安装”），建账户、放配置、装服务：

```sh
# 专用账户：不能登录、没有家目录
sudo useradd --system --user-group --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin rcs-forwarder
# 配置和身份：属于 root，只有这个账户读得到（目录 0750，文件 0640）
sudo install -d -m 0750 -o root -g rcs-forwarder /etc/rcs-forwarder
sudo rcs_forwarder --generate-identity /etc/rcs-forwarder/identity.key     # 打印两端要的 SHA-256
sudo chgrp rcs-forwarder /etc/rcs-forwarder/identity.key && sudo chmod 0640 /etc/rcs-forwarder/identity.key
sudo install -m 0640 -o root -g rcs-forwarder /usr/local/share/rcs-forwarder/forwarder.conf.example \
     /etc/rcs-forwarder/forwarder.conf
sudoedit /etc/rcs-forwarder/forwarder.conf     # 入口；secret 换成 1:$(openssl rand -hex 32)，换之前转发器不启动
# 服务
sudo install -m 0644 /usr/local/share/rcs-forwarder/rcs-forwarder.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now rcs-forwarder
```

日常：

```sh
systemctl status rcs-forwarder              # 运行没有、最近的事件行；起不来时是哪一项错了
journalctl -u rcs-forwarder -f              # 事件行（没写 log_file 时都在这里）
sudo systemctl restart rcs-forwarder        # 换了配置、身份或令牌密钥后（配置只在启动时读）
sudo systemctl reload rcs-forwarder         # SIGHUP：重新打开 log_file，别的不变，连着的端不受影响
sudo systemctl disable --now rcs-forwarder  # 停下，开机也不再运行
```

服务（`rcs-forwarder.service`）开机就运行，失败了 5 秒后重来；停止时 systemd 发 SIGTERM，转发器告诉每一端“它要走了”（观看端重连，常驻监听的分享端重新连上）。它以自己的账户运行，加固到只剩它要的：

- `NoNewPrivileges`，没有任何能力（`CapabilityBoundingSet=` 空）；
- `ProtectSystem=strict`：整个文件系统只读，只有 `/var/log/rcs-forwarder` 可写（`LogsDirectory=` 建它、交给这个账户，等于把它加进 `ReadWritePaths=`）；`ProtectHome`、`PrivateTmp`、`PrivateDevices`，看不到别的进程（`ProtectProc=invisible`）；
- 只许 IPv4、IPv6 和 Netlink（读网卡）套接字，系统调用只许 `@system-service` 去掉 `@privileged`、`@resources`，`MemoryDenyWriteExecute`，不能建命名空间、不能改内核参数和时钟；
- `LimitNOFILE=65536`、`TasksMax=4096`：每个端两个线程，TCP、WebSocket 的端还有一个连接；`max_ends` 调到 1024 以上时一起调大 `TasksMax`。

`systemd-analyze security rcs-forwarder` 列出每一项（Ubuntu 24.04 上总评 1.3 “OK”，剩下的分主要是它要联网）。宿主自己的改动放在 drop-in 里（`sudo systemctl edit rcs-forwarder`），装新版本时换掉的 `rcs-forwarder.service` 不会冲掉它：

- **1024 以下的端口**（例如不放代理、直接听 443）：只加这一个能力，而且只在这时加：

  ```ini
  [Service]
  CapabilityBoundingSet=CAP_NET_BIND_SERVICE
  AmbientCapabilities=CAP_NET_BIND_SERVICE
  ```

  不加时听这样的端口启动就失败（`permission denied`，`systemctl status` 里看得到）。放在代理后面时用不着它：代理听 443，转发器听 47996。
- **别的位置**：程序装在别处时先写一行空的 `ExecStart=` 再写新的；`log_file` 写在 `/var/log/rcs-forwarder/` 之外时加 `ReadWritePaths=<那个目录>`。

日志文件：不写 `log_file` 时事件行进 journal，journald 自己轮换。写进文件时（`log_file = /var/log/rcs-forwarder/forwarder.log`，文件 0640）装上 logrotate 的规则，每周轮换、留 4 份压缩的，轮换后 `systemctl reload` 让转发器重新打开文件（打不开时接着写原来的，并在里面写一行原因）：

```sh
sudo install -m 0644 /usr/local/share/rcs-forwarder/rcs-forwarder.logrotate /etc/logrotate.d/rcs-forwarder
sudo logrotate -d /etc/logrotate.d/rcs-forwarder     # 只看它会做什么
```

卸载：`sudo systemctl disable --now rcs-forwarder`，删掉 `/etc/systemd/system/rcs-forwarder.service`、`/etc/logrotate.d/rcs-forwarder`、`/etc/rcs-forwarder/`、`/var/log/rcs-forwarder/` 和装出的文件（`build/linux-forwarder/install_manifest_forwarder.txt` 列着），`sudo systemctl daemon-reload`，`sudo userdel rcs-forwarder`。

### 容器

`server/forwarder/deploy/Dockerfile`：Ubuntu 24.04，在容器里按 `linux-forwarder` 编出程序，只把程序、库、样例和许可证留在镜像里，以 990 号账户运行，事件行进 `docker logs`，`docker stop` 发 SIGTERM。

```sh
docker build -f server/forwarder/deploy/Dockerfile -t rcs-forwarder .     # 在仓库的根目录
docker run -d --name rcs-forwarder --restart unless-stopped \
    -p 47995:47995/udp -p 47995:47995/tcp -p 127.0.0.1:47996:47996/tcp \
    -v /etc/rcs-forwarder:/etc/rcs-forwarder:ro rcs-forwarder
```

容器里的配置听 `0.0.0.0`（WebSocket 入口也是，它的端口只发布在宿主机的环回上，给那里的 TLS 代理）；`/etc/rcs-forwarder` 里的文件让 990 号账户读得到（`chown root:990`、`chmod 0640`，目录 0750）。宿主机上没装程序时，用镜像生成身份：`docker run --rm --user 0 -v /etc/rcs-forwarder:/out rcs-forwarder --generate-identity /out/identity.key`（以 root 写出，再照上面改属组和权限）。

### macOS：launchd

1. 安装：`sudo cmake --install build/release --component forwarder --prefix /usr/local`。
2. 建一个专用账户（例如 `_rcsforwarder`，用 `dscl` 建、不能登录），配置和身份放在 `/usr/local/etc/rcs-forwarder/`：目录属于 root，`forwarder.conf` 和 `identity.key` 只让这个账户读（`chown root:_rcsforwarder`、`chmod 640`）。日志目录 `/usr/local/var/log/` 让它能写。
3. 把 `server/forwarder/deploy/rcs-forwarder.plist` 改成宿主自己的标签和路径，复制到 `/Library/LaunchDaemons/`（属于 root，0644），然后：

```sh
sudo launchctl bootstrap system /Library/LaunchDaemons/com.example.rcs-forwarder.plist   # 加载并启动
sudo launchctl kickstart -k system/com.example.rcs-forwarder                             # 换了配置后重启
sudo launchctl bootout system/com.example.rcs-forwarder                                  # 停止并卸载
```

开机就运行、退出了就重新拉起（`KeepAlive`）。停止时 launchd 发 SIGTERM，转发器告诉每一端“它要走了”（端按断线处理：观看端重连，常驻监听的分享端重新连上）。macOS 的应用防火墙打开时，第一次运行会问是否允许它接受入站连接，或者由管理员预先放行这个程序。

### Windows：服务

1. 安装：`cmake --install build\windows --component forwarder --prefix "C:\Program Files\rcs-forwarder"`。
2. 配置和身份放在 `C:\ProgramData\rcs-forwarder\`，只让服务的账户和管理员读：`icacls C:\ProgramData\rcs-forwarder /inheritance:r /grant "NT AUTHORITY\LOCAL SERVICE:(OI)(CI)M" /grant "Administrators:(OI)(CI)F"`。配置里写 `log_file`（服务没有控制台）。
3. 用管理员的命令行注册、启动：

```bat
sc.exe create rcs-forwarder binPath= "\"C:\Program Files\rcs-forwarder\bin\rcs_forwarder.exe\" --service --config C:\ProgramData\rcs-forwarder\forwarder.conf" start= auto obj= "NT AUTHORITY\LocalService"
sc.exe failure rcs-forwarder reset= 86400 actions= restart/5000/restart/5000/restart/5000
sc.exe start rcs-forwarder
sc.exe stop rcs-forwarder
```

`--service` 让程序按服务控制管理器的约定运行：报告“运行中”，收到停止或关机时停下转发器（端的感受同上）再报告“已停止”；配置有错时服务以错误停下，原因写在 `log_file` 里。不由服务控制管理器启动时 `--service` 以错误退出，排查配置时去掉 `--service` 在命令行里跑。防火墙规则由管理员决定，例如 `New-NetFirewallRule -DisplayName "rcs-forwarder" -Direction Inbound -Program "C:\Program Files\rcs-forwarder\bin\rcs_forwarder.exe" -Action Allow`。

## 经转发器的各项功能

转发器只复制画面和分享端的声音（用流密钥封装，上传一次）。其余的一切：控制和输入、指针、数据通道、剪贴板（文字、图片、文件）、文件传输、观看端的声音（麦克风或宿主推的）、流公告，都走每个观看端和分享端之间端到端的连接，转发器把它当作管道的数据报原样转过去。所以这些功能经转发器时和直连时用法一样，宿主不用为转发器改什么。要知道的两点：

- 管道每个方向限 `max_pipe_bps`（默认 50 Mbit/s），经转发器的文件传输和大的剪贴板内容最快就是这么快（下面测到每个方向约 5.9 MB/s）。端到端的连接靠丢包找到这个上限：大文件传着时，管道里一成多的数据报被转发器丢掉、端到端重传，`rcsfwd_room_stats.pipe_dropped` 记着。局域网里要传得更快，调高它。
- 那一跳上管道的数据报和声音一样排在画面前面，但画面只为它们让出一次突发的时间：大文件传着时画面照常。

在 Mac 本机环回上测过（会话场景，经转发器的 UDP、TCP、WebSocket 入口；每个连跑 5 次，再在每个核一个 `yes` 占满 CPU 时跑 3 次，都过，单个 2～13 秒）：

- `forwarder_control`：两个观看端（WebSocket、UDP）在同一个房间，宿主按令牌的路由号让一个控制。它一次发出的 252 个移动、按钮、滚轮、按键、文字（含中文），分享端按原样、按顺序收到并执行；另一个发不出（`INVALID_STATE`），宿主交给它后它的按键到了；从第一个收回控制（它按住的松开），再从所有人收回。指针和画面分开（`RCS_CURSOR_SEPARATE`）时，形状只来一次，位置跟着动。
- `forwarder_clipboard`：两个观看端（TCP、WebSocket）。分享端的文字和 3 MiB 的图片到两个观看端；第一个观看端的文字和 1 MiB 的图片到分享端、再到另一个，不回传。分享端复制的文件，第一个观看端粘贴到目录（像 Finder），另一个按文件读（像资源管理器）；第一个观看端复制的文件，分享端粘贴。全程是测试自己的剪贴板，不碰系统的。
- `forwarder_files`：两个观看端（UDP、WebSocket）。目录（空文件、空目录、子目录）双向传；300 个小文件 0.1 秒；32 MiB 同时下到两个观看端、各从两个观看端上传 16 MiB，SHA-256 都一致，5.7 秒传完（经转发器合计 17.6 MB/s）。其间画面约 31 FPS、最长 67 ms 没有新画面；UDP 的观看端每 50 ms 按一次键，到分享端平均 0.2 ms、最多 1.1 ms。
- `forwarder_files_resume`：观看端到转发器的路断了（UDP，中间的代理全丢），转发器 5 秒后放走它，两端的传输等着；路通了它经转发器重连（分享端看是新的观看端编号），两个方向的传输从写到的地方接着传完。然后观看端这次运行中途结束，下一次运行用这次传输的令牌再发同样的东西，只传缺的部分。
- `forwarder_microphone`：一个观看端（UDP）推自己的声音（750 Hz），另一个（TCP）用麦克风（测试的，500 Hz）。分享端按观看端分别听到，频率、响度和发出的一样；两个观看端同时听得到分享端只编一次的声音。一个停发、分享端停收，对方都知道。播放进静音设备，没有声音出来。

## 一台转发器带多少人

实测见 [PERFORMANCE](../PERFORMANCE.md) 的“转发器的吞吐”（ARM64 的密码学指令和批量发送合起来、TCP 和 WebSocket 入口一批一次写，以及之前的几节）：Mac（M4 Pro）本机环回和一台 2 个虚拟核的 ARM64 Ubuntu 虚拟机，负载是 `tools/rcsdk_forwarder_load`。**这些不是公网的数字**：环回和虚拟网卡几乎不丢包，公网上丢包时重传和校验（FEC）多花 CPU 和带宽，真实网卡每个包的代价也不同（虚拟机里最大的一项是虚拟网卡每批通知一次宿主）。上线前在目标机器上用这个工具量一遍（[用法](../../tools/README.md#转发器的负载)）。按那些数字规划：

- **CPU**：ARM64 的 Linux 上，处理器有 ARMv8 的 AES、SHA-256 指令时（服务器的 ARM64 都有；SDK 用它们做逐跳加密，Linux 上一批数据报一次 `sendmmsg`），每个观看端约 0.45% 个核（1080p30 约 4 Mbit/s，带声音）、约 0.6% 个核（1080p60 约 8 Mbit/s），即出口每 Mbit/s 约 0.065～0.1% 个核，一个核约 1～1.5 Gbit/s。没有这些指令的处理器（例如树莓派 4）用 BearSSL 的软件，约 0.8% 和 1.25%（出口每 Mbit/s 约 0.13～0.19%），一半以上的 CPU 是加密。按核线性（软件密码学时 1 个和 2 个核量过）。房间多、每间人少时每人更贵：每间 3 人时约是一个大房间的 1.4～1.65 倍（每个分享端的上传也要解开、重组，也各两个线程）；TCP、WebSocket 入口 50 人时和 UDP 差不多，150 人时多约四分之一（2 核虚拟机里 79% 对 64%：一轮的数据报也是一次写出，多的是 TCP 自己的分段、确认和入口读连接的那个线程），按每个这样的观看端约 1.25 个 UDP 的观看端规划；声音按包算：分享端 5 ms 一包（Mac 的 Core Audio 进程捕获）比 10 ms 一包多约四分之一的 CPU。CPU 到九成左右之前延迟只是慢慢变长，用满就崩成秒级延迟、掉帧（房间多时先崩在接收：那一个接收线程排不上，接收缓冲溢出、丢画面）：按 75% 规划。
- **带宽**：出口 ≈ 观看端数 × 码率 × 1.1，入口 ≈ 分享端数 × 码率 × 1.1。千兆网卡约 220 人 × 4 Mbit/s 或 110 人 × 8 Mbit/s 就满了，而 2 个核按 75% 能出 1.5～2.2 Gbit/s：网卡远先满，要 2.5G、10G 的网卡（云主机看实例的网络带宽上限）。
- **内存**：约 20 MB，加每人约 0.85 MB（4 Mbit/s）～1.4 MB（8 Mbit/s，主要是那一跳为重传留的 1 秒画面）；过载时每个跟不上的观看端最多再攒 8 MB 才丢，按最坏算是人数 × 8 MB。TCP、WebSocket 的连接每条再多约 70 KB（待发的缓冲留着一批那么大，最多 256 KiB）。每个端两个线程。
- **一个房间人多时的延迟**：同一张画面同时发给每个人，后面的人等前面的。有密码学指令和批量发送时这一项很小：2 核上一个房间 50～250 人 p50 都是 4～6 ms、p95 8～12 ms；软件密码学时约每人 0.05～0.1 ms（200 人 p50 约 13 ms）。同样多的人分在多个房间里时没有这一项。

| 服务器（ARM64，按 75% 外推） | 1080p30 约 4 Mbit/s 的观看端 | 1080p60 约 8 Mbit/s 的观看端 | 出口 |
| --- | ---: | ---: | --- |
| 2 核 | 约 330（实测：一个房间 250 人 111%、p95 12 ms；两个房间共 350 人 158%、p95 16～18 ms；400 人 180%、p95 24～31 ms；450 人 p95 0.5 秒，500 人崩） | 约 250（实测：一个房间 250 人 150%、p95 14～16 ms；两个房间共 300 人 179%、p95 16 ms；350 人崩） | 约 1.5～2.2 Gbit/s：千兆网卡不够 |
| 4 核 | 约 650 | 约 500 | 约 3～4.4 Gbit/s |
| 8 核 | 约 1300 | 约 1000 | 约 6～9 Gbit/s |
| 2 核，没有 ARMv8 的密码学指令 | 约 170（实测：150 人 130%、p95 17 ms；200 人 173%、p95 25～27 ms；250 人崩） | 约 100（实测：100 人 141～145%、p95 11～12 ms；125 人 176%） | 约 0.75～0.9 Gbit/s |

每间 3 人的小房间：2 核约 65 个（70 个 154%、p95 8～9 ms；85 个崩）。4 核、8 核是按 2 核的斜率外推的：一个 UDP 端口只有一个接收线程，几百个端时每端两个线程，再往上能不能线性没量；x86-64 上 AES 用 AES-NI、SHA-256 还是 BearSSL 的软件，没量。`max_viewers_per_room` 最大 256；`max_ends`、`max_rooms` 照上表留余量设，超出的端以 `BUSY` 被拒，宿主可以把房间分到另一台。看 `/proc/net/snmp` 的 UDP `RcvbufErrors`：涨了说明接收缓冲不够（转发器要 4 MB，Linux 默认最多给约 200 KB，`sysctl net.core.rmem_max`、`wmem_max` 可以放大），或者 CPU 满了、接收线程排不上（我们的测试里只在过载时涨过：故意的固定码率，85 个房间 × 3 人）。macOS 上的转发器（局域网里的桌面机）一批在锁下发，本机环回上一个 UDP 端口到 1.1～1.3 Gbit/s 还不丢，CPU 比 Linux 虚拟机多（系统的密码学，250 人 × 4 Mbit/s 约 2.2 个核）：大群用 Linux。macOS 上经 TCP、WebSocket 入口的观看端反而比经 UDP 的便宜（每条连接一批一次写；100 人 × 8 Mbit/s 约 1 个核，UDP 约 1.15 个核）。

## 运行中看什么

- 程序：事件行。库：`rcsfwd_get_stats`（房间、端、TCP 和 WebSocket 连接数、被拒和违规的、5 秒不报到的、留着的字节）、`rcsfwd_get_room_stats`（上传和发出的画面、声音包、按规则丢的、晚来的人从留着的画面开始的次数、为等着的人向分享端要的关键帧、分享端上行丢的画面、留着的字节、当场下移）、`rcsfwd_list_viewers`（每个观看端的入口、档、声音、路径的目标码率、丢包、往返、积压、发出的帧和字节）。
- 两端：`RCS_STATS_FORWARDER`（[14](14-events-stats-logging.md)）：怎样连的、那一跳的往返和字节、分享端上行量到的码率和它限住编码的次数、流密钥的代和步。

## 还没量的

- 吞吐：只在 Mac 本机环回和 2 核的 ARM64 虚拟机里量过（上面）；公网、真实服务器的网卡（能自己切 UDP GSO 的）、x86-64、4 核以上、没有密码学指令的 ARM64 真机、几小时的长跑、上千个端都没量；Windows 上的转发器不批量发送。
- 控制、剪贴板、文件传输、观看端的声音经转发器只在本机环回上测过（上一节）：真实网络上（丢包、延迟）大文件传着时的输入延迟和画面、文件传得多快，iOS、Android 和浏览器的端经转发器用剪贴板和文件传输，都还没试过。
- 浏览器经 WebSocket 入口只在本机用会话场景（和浏览器包同一条路，测试自己开的 WebSocket）和 `serve.mjs` 的转发测过，没在浏览器里跑过。
- Linux：Ubuntu 24.04 arm64（虚拟机）上 GCC 13.3 和 Clang 18 编译通过，`linux-forwarder-scenarios` 的测试全过（转发器自己的、`wss://`、经它的 9 个会话场景），照上面的步骤装成 systemd 服务跑过：环回上 `rcsdk_cli` 的分享端经 TCP、观看端经 `ws://` 和 UDP 同时看（720p30），`reload` 和 logrotate 之后写进新文件，1024 以下的端口不加能力时失败、加了 drop-in 后听得上。x86-64 和 Ubuntu 26.04 还没跑过（CI 的 `forwarder-linux` 作业跑 GitHub 的 Ubuntu 24.04 x86-64 和 arm64，容器由 `forwarder-container` 构建）。
- macOS 只在本机环回上测过；Windows 版只交叉编译过，服务模式没在真机上跑过；公网（云服务器）、真实的 nginx / Caddy 代理后面没跑过（测试用的是 Python 的 TLS 终止器）。
