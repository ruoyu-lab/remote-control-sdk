# 23 浏览器观看端

SDK 的核心编成 WebAssembly，在网页里当观看端：同一套协议、加密、码率控制和丢包修复，用浏览器的 WebCodecs 解码。分享端不用为浏览器做任何事，只从对端的平台值（`RCS_PLATFORM_WEB`）知道对方在浏览器里。

**现在能做的**：H.264、HEVC、VP8、VP9、AV1 解码（硬件或软件、4:2:0 / 4:4:4、8 / 10 位，按浏览器试出）；解码、画面和 WebSocket 都在 SDK 的媒体线程上，页面主线程忙时画面照常；多路画面（分享端的每块显示器一个画布）；声音（Opus，AudioWorklet，抖动缓冲和原生相同）和麦克风回传；鼠标、滚轮和触控板、按键、输入法和软键盘的文字、触摸、笔、指针锁定下的相对移动；单独传送的远端指针；剪贴板（文字、HTML、图片，两个方向）和粘贴；文件和目录的传输（两个方向，可以续传）；数据消息；暂停、恢复、要关键帧、运行中改要求的画质。实测在 Mac 上的 Chromium 152（一个内嵌浏览器）和正式版 Chrome、Edge，见本章最后。

连接只经页面打开的通道：直接到分享端（`RCS_TRANSPORT_HOST`，示例用 WebSocket，宿主也可以接自己的通道，WebTransport 等），或者到转发器（人多的群，下面的[经转发器](#经转发器)）。SDK 不为浏览器内建 WebRTC 或 P2P 直连。

## 编出来

要 Emscripten SDK（`EMSDK` 环境变量，或者装在 `~/emsdk`）：

```bash
cmake --preset wasm
cmake --build --preset wasm --target rcsdk_web
```

得到 `build/wasm/web/` 里的四个文件：`rcsdk.mjs`（页面导入的 ES 模块）、`rcsdk_core.mjs` 和 `rcsdk_core.wasm`（SDK 本身，带剪贴板和文件传输两个官方模块）、`rcsdk_audio.js`（声音的 AudioWorklet）。放在同一个目录里由网站提供。`scripts/build-web.sh` 编好后打成 npm 包 `build/wasm/remote-control-sdk-<版本>.tgz`（带 `rcsdk.d.ts` 类型声明、`LICENSE` 和第三方声明），宿主 `npm install` 这个文件即可；要不要发布到 npm 由你定。`ctest --preset wasm` 在 node 里跑可移植的测试（浏览器里没有的套接字、本机端点和 AAC 那几项不编或跳过）。

## 页面要什么

- **跨源隔离**：SDK 用多线程（`SharedArrayBuffer`），提供页面的响应要带 `Cross-Origin-Opener-Policy: same-origin` 和 `Cross-Origin-Embedder-Policy: require-corp`。没有时 `load()` 失败并说明。
- **安全上下文**：`https`，或者本机的 `http://localhost`。
- **WebCodecs 和 OffscreenCanvas**：Chrome / Edge 94 起、Safari 16.4 起、桌面版 Firefox 130 起；Android 上的 Firefox 没有 WebCodecs。
- **连接**：浏览器没有自己的套接字（`rcs_capabilities.transports` 只有 `HOST`），会话走页面的通道（见下面“通道”）。在浏览器里选 UDP、TCP、P2P、中继，`start` 返回 `UNSUPPORTED`。

## 页面里的密钥：不放第三方脚本，用严格的 CSP

WASM 的内存是一块 `SharedArrayBuffer`，页面上的任何脚本都读得到它，也读得到页面自己的 JS 变量。页面被注入脚本（XSS）或带着第三方脚本（统计、广告、客服插件、来源不受你控制的 CDN）时，连接密钥、PIN、口令和画面就都可能泄露。所以观看端的页面：

- 不放第三方脚本；自己的脚本从自己的源提供，用 Subresource Integrity 固定版本。
- 带严格的 `Content-Security-Policy` 响应头。包本身需要的是：自己源的脚本和 Worker（`rcsdk_core.mjs` 用自己的地址开 Worker，没有 `eval`、`new Function` 或 blob 脚本）、编译 WebAssembly（`'wasm-unsafe-eval'`）、`rcsdk_audio.js` 这个 AudioWorklet、`data:` 图像（单独传送的远端指针变成 `data:` 的 CSS 指针）、连到中继的 WebSocket。起点可以是下面这条，把 `wss://relay.example.com` 换成你的通道；它按包的代码推出，还没有在各个浏览器里逐项实测，上线前在目标浏览器里打开控制台确认没有 CSP 报错：

```
Content-Security-Policy: default-src 'none'; script-src 'self' 'wasm-unsafe-eval'; worker-src 'self';
  connect-src 'self' wss://relay.example.com; img-src 'self' data:; style-src 'self';
  base-uri 'none'; form-action 'none'; frame-ancestors 'none'
```

- SDK 这边：密钥、PIN 或口令不经过选项的 JSON，而是单独以字节交给模块，调用一结束就在模块内存里清零后释放（`createSession` 为 PIN、口令编码出的那份副本也清零）；胶水层自己的副本同样清零。会话活着时核心留着一份（重连要用），随会话销毁释放（这份不清零）；页面自己传进来的 `key`（`Uint8Array`）和字符串由页面处理，JS 的字符串无法清零，用完别留在全局变量里，也别写进 URL 或 `localStorage`。
- 这些只缩小窗口，不能挡住已经在页面里运行的恶意脚本：它可以在你调用 `createSession` 之前截下参数。真正的防线是不让别人的脚本进页面。

## 用法

```js
import { load, VideoView, InputMapper, CursorView, ClipboardSync, collectFiles } from './rcsdk.mjs';

const sdk = await load();                       // 加载模块，试出浏览器能解什么（sdk.decoders）
const session = sdk.createSession({
  key,                                          // 32 字节的连接密钥（Uint8Array）；或 identity: { pin } / { password }
  decoder: { codecs: ['h264', 'hevc', 'vp9', 'av1'], formats: ['420_8'], acceleration: 'hardware' },  // 都可省略
  viewRequest: { width: 1920, height: 1080, framesPerSecond: 60, cursor: 'separate' },   // 可选
  sendAudio: { microphone: true },              // 可选：页面的麦克风发给分享端
  clipboard: { send: true, receive: true },     // 可选：剪贴板模块
  files: { send: true, receive: true },         // 可选：文件传输模块
});
const view = new VideoView(session, canvas);    // 画在媒体线程上，只画最新的一帧
const input = new InputMapper(session, view);   // 鼠标、滚轮、按键、文字、触摸、笔、粘贴
new CursorView(session, view);                  // cursor 'separate' 时把远端指针变成页面的指针
new ClipboardSync(session);                     // 分享端复制的内容写进页面的剪贴板
session.addEventListener('state', (e) => console.log(e.detail.state, e.detail.message ?? ''));
await session.connectWebSocket('wss://example.com/relay?room=r1');  // 页面的通道（在媒体线程上）
await session.start();
console.log(session.stats().videoReceive);      // decodedFrames、lastDecodeNs……
await session.destroy();
```

- 名字是 C 接口的驼峰写法；事件名是 `rcs_event_type` 去掉前缀的小写（`state`、`stream`、`codec`……），`detail` 是对应的字段（都带 `viewerId`、`stream`、`timestampNs`），枚举是 C 的数值；状态、编码格式、画面格式、指针方式给名字。完整的类型在 `rcsdk.d.ts`。
- 会等的调用（`start`、`stop`、`destroy`、`updateRequest`）返回 Promise，在 SDK 自己的线程上执行，页面的主线程从不等 SDK。失败时抛 `RcsError`（`result` 是 `rcs_result`，`message` 是原因）。
- `sdk.decoders` 是这个浏览器能解的格式（每种编码格式、硬件和软件各一组画面格式），`decoder.codecs` 默认就是它们（H.264 在前）。
- 其他调用：`sendData(channel, bytes, { unreliable })`（分享端的以 `message` 事件到达）、`pause(['video'])` / `resume`、`requestKeyframe()`、`updateRequest({ framesPerSecond: 30 })`、`stats({ stream })`（按节：`videoReceive`、`network`、`audioReceive`、`audioSend`、`input`、`data`）、`info({ stream })`（一路画面最近的 `rcs_stream_info`、对端、控制权、桌面）。

## 线程

页面主线程只处理事件和页面自己的调用，数据面全在 SDK 的线程上：

| 线程 | 做什么 |
| --- | --- |
| 页面主线程 | 事件、输入（`sendInput` 立即返回）、指针形状、麦克风的 `getUserMedia`、剪贴板的读写 |
| 媒体线程（一个 worker） | `connectWebSocket` 的 WebSocket、WebCodecs 的解码器、`VideoView` 的画布（`OffscreenCanvas`）：画面从网络到屏幕不经页面主线程 |
| 文件线程（一个 worker） | 文件传输读页面的文件、往目录或内存里写 |
| API 线程 | 会等的调用 |
| AudioWorklet | 播放和录音，各自和 SDK 的线程经共享内存里的环交换 |

页面的脚本很忙时（大量 DOM 操作、长任务），画面、声音和网络照常；实测页面主线程卡住 3 秒时，1080p60 照样收、解、画了 180 帧。

## 画面和多路画面

- `new VideoView(session, canvas, { stream })`：画布交给媒体线程（`transferControlToOffscreen`），之后由那里画。页面只管画布的布局：CSS `object-fit: contain` 保持比例，`geometry(clientX, clientY)` 按同样的方式把页面坐标换成画面的 0～65535。`width`、`height`、`drawn` 是媒体线程写的，随时可读。
- 一块画布交给媒体线程后就一直是它的：`view.close()` 之后同一块画布可以再给新的 `VideoView`；不再用了调 `VideoView.releaseCanvas(sdk, canvas)`。
- **多路画面**：分享端用 `rcs_streams_config` 分享几块显示器时，每块是一路（0、1、2、3），各有自己的 `stream` 事件（`info.desktop` 是那块显示器在分享端桌面上的位置，用来按原样排列画布；`ended` 为真时那一路结束）。每路一个 `VideoView`（`{ stream: n }`）；`InputMapper` 发的输入、`CursorView` 显示的指针都按它的视图的那一路。`viewRequest.streams: [0, 2]` 只要其中几路。
- **自己画**：设 `session.onframe = (frame, info) => { ...; frame.close(); }`，每帧的 `VideoFrame` 交到页面（`info.stream` 说是哪一路），可以画进 WebGL / WebGPU；不设时只有 `VideoView` 画。

## 输入和粘贴

`InputMapper(session, view, { touch, paste })` 把视图上的输入发给那一路：鼠标（指针锁定时相对移动，`lock()` 在点击里调）、滚轮（有刻度的发 `WHEEL`，触控板发 `SCROLL`）、按键（按位置发 HID 码）、文字（输入法和软键盘的以 `TEXT` 一个字符一个事件）、触摸（`touch: 'mouse'` 时第一根手指当鼠标）、笔。离开页面时放开按住的键。

Ctrl+V 或 Cmd+V 按 `paste`：

- `'clipboard'`（默认）：先把页面剪贴板里的内容作为剪贴板模块的一份复制发出去，确认送达后再发 V 键（期间按下、松开的键按原来的顺序排在后面），分享端粘贴的就是页面里的内容。会话没开剪贴板发送时只发键。
- `'text'`：把文字以 `TEXT` 打出来（先放开分享端那边的 Ctrl / Cmd），给没有剪贴板模块的分享端，比如登录框。
- `'key'`：只发键，分享端粘贴它自己剪贴板里的。

## 剪贴板

`createSession({ clipboard: { send, receive, formats, maxBytes, sendExisting } })` 开剪贴板模块（`rcsdk/clipboard.h`），目标是宿主（`RCS_CLIPBOARD_HOST`）：浏览器不让页面随时读写系统剪贴板，读写由页面在允许的时候做。

- 分享端复制的内容以 `clipboard` 事件到达：`{ text, html, rtf, image (image/png 的 Blob), concealed }`。
- `session.clipboard.send({ text, html, rtf, image })` 发一份复制，送达后 Promise 完成（`clipboardcopy` 事件也说）；图片可以是浏览器能解的任何格式，按 PNG 发。`session.clipboard.info()` 是分享端收哪些格式和这次运行的计数。
- `ClipboardSync(session, { write, readOnFocus })` 把它和页面的剪贴板接起来：分享端的复制写进页面剪贴板（`navigator.clipboard.write`：页面有焦点时 Chromium 立刻写；不行时在页面下一次点击或按键时写，Safari、Firefox 要这样）；`readOnFocus: true` 时页面每次得到焦点就读剪贴板（浏览器第一次会问用户）发给分享端，于是在别处复制、回到页面后在远端用菜单粘贴也行。
- 两个方向都意味着信任对方：`receive` 打开、接了 `ClipboardSync` 时，分享端想替换观看端的系统剪贴板几次就能替换几次（浏览器会清洗 HTML 和图片，但文字照原样），用户随后在本机别处粘贴的就是对方给的内容；`readOnFocus` 每次得到焦点都把本机剪贴板里不管什么内容（可能是刚复制的口令）发给分享端。只在信任分享端时打开，或者让用户每次自己决定（不用 `readOnFocus`，由页面在用户操作时调用 `session.clipboard.send`）。

## 文件传输

`createSession({ files: { send, receive, maxItems, maxOfferBytes, offerTimeoutMs, reconnectWaitMs, maxBytesPerSecond } })` 开文件传输模块（`rcsdk/file_transfer.h`），目标是宿主：读写都在文件线程上，用页面给的 `File` 和目录。

- **发**：`await collectFiles(source)` 把拖放（在 `drop` 处理里传 `DataTransfer`）、文件选择框（`<input webkitdirectory>` 也行）或 `FileSystemHandle` 变成条目，目录带着里面的一切；`session.files.send(entries)` 返回传输编号。条目也可以自己造：`{ name: '相对路径', kind: 'file' | 'directory', file: Blob }`。
- **收**：分享端发来时有 `fileoffer` 事件（`transfer`、`items`、`totalBytes`；`session.files.get(transfer)` 列出每一项），`session.files.accept(transfer, { into, conflict })` 收下：`into: 'memory'`（每个文件以 `filereceived` 事件的 `File` 交给页面）或一个 `FileSystemDirectoryHandle`（用户选的 `showDirectoryPicker()`，或源私有文件系统 `navigator.storage.getDirectory()`）；`conflict` 是同名时 `'rename'`（默认，`名字 (1).扩展名`）、`'overwrite'` 或 `'fail'`。不要就 `reject(transfer, reason)`。
- 过程：`fileprogress`、`fileitemdone`、`filedone`、`filestate` 事件；`cancel`、`pause`、`resume`、`get`、`peer()`。
- **续传**：连接断了、会话结束了，下一次运行用同样的条目和原来的令牌再发（`send(entries, { resumeToken })`，令牌在 `get(transfer).transfer.token`），已经写好的部分不再传。页面收的时候：源私有文件系统里写下的部分会保留，下一次从那里接着写；用户选的目录经可写流写，一个文件写完才出现，没写完的下一次重来，写完的不再传。

## 麦克风

`sendAudio: { microphone: true }` 时会话开始发声音，浏览器在那时问用户要不要给麦克风（`getUserMedia`，回声消除和降噪是浏览器的，`voiceProcessing: false` 关掉）。用户拒绝时 `audio` 事件说明原因（`PERMISSION_REQUIRED`）；只能用浏览器的默认麦克风。

## 通道

- `connectWebSocket(url)`：WebSocket 开在媒体线程上，收到的直接交给 SDK。关掉时有 `socketclose` 事件。
- 自己的通道：不调 `connectWebSocket`，收到的字节交给 `session.transportReceive(bytes)`，结束时 `session.transportClosed()`，要发的字节来到 `session.ontransportsend = (bytes) => 通道里还没发出的字节数`。默认是字节流；`createSession({ channel: 'datagram' })` 时是数据报（WebTransport 的数据报那样的通道：每次一个完整的包）。

## 经转发器

人多的群里分享端经转发器（[09](09-transports.md#经转发器rcs_transport_forwarder)、[24](24-forwarder.md)）分享时，浏览器也经它看：页面的通道连转发器的 WebSocket 入口（`ws://`，公网上是它前面的 TLS 代理 `wss://`），那一跳（核对转发器的公钥、令牌、重传、冗余、节流、动态码率）和端到端的连接都在 SDK 的 WebAssembly 里跑，和原生的观看端是同一段代码：晚来时从转发器留着的关键帧开始，路慢时转发器当场把它移到低一档，声音和别人共用一份编码。对应 C 接口是 `RCS_TRANSPORT_FORWARDER` 加链路 `RCS_FORWARDER_HOST`（宿主给的通道，`rcs_capabilities.forwarder_links` 在浏览器里只有它）。

```js
// 页面向自己的服务器要这个观看端的令牌（放在请求体里，不放 URL）：
const given = await (await fetch('/forwarder-token', { method: 'POST', body: JSON.stringify({ room }) })).json();
const session = sdk.createSession({
  key,                                              // 端到端的秘密照旧：连接密钥、PIN 或口令
  forwarder: {
    keySha256: given.keySha256,                     // 转发器公钥的 SHA-256（32 字节，或 64 个十六进制数字）
    token: given.token,                             // 宿主服务器签的 rcsf1 订阅令牌
    refreshToken: async () => (await fetch(...)).token,   // 可选：重连前要新的
  },
});                                                  // 通道默认 'datagram'：WebSocket 入口每条消息一个数据报
session.addEventListener('socketclose', () => setTimeout(() => session.connectWebSocket(url), 1000));
await session.connectWebSocket('wss://forwarder.example.com/rcs');
await session.start();
console.log(session.stats().forwarder);             // peer、mediaFrames、mediaUnkeyed、hopRoundTripNs……
```

- **令牌**：由宿主的服务器签（[24](24-forwarder.md#令牌)），页面在请求体里要、拿到后交给 `createSession`，不写进 URL、`localStorage`。它随选项进模块，模块内存里的那份用完清零，会话活着时核心留着一份（重连要用）。
- **重连**：一条 WebSocket 就是一个那一跳。它断了（`socketclose`）页面再开一条，会话按重连设置（`reconnect`，默认 30 秒内）在新的那条上重新连转发器；连之前调 `refreshToken`（页面的异步函数，最多等 30 秒；返回 `null`、抛错、超时都沿用原来的令牌，过期了转发器会拒绝，`state` 事件写明原因）。
- **CSP**：`connect-src` 写转发器（或它前面代理）的 `wss://` 地址；页面和代理同源时 `'self'` 就够。
- 页面自己的通道也行（`transportReceive` 等，见上面“通道”）：比如宿主自己的服务器转一道。数据报的通道一个数据报一条消息，字节流的通道要接到转发器的 TCP 入口（两字节长度的分帧正是它的）。

示例：`examples/web/forwarder.html` 和 `serve.mjs --forwarder`（见下面“示例”）。在 Mac 上的 Chromium 152（一个内嵌浏览器）里照下面的步骤跑过（见“验证到哪里”）；在 Mac 上试：

```bash
build/release/bin/rcs_forwarder --generate-identity id.key        # 打印身份的 SHA-256：SHA
echo "7:$(openssl rand -hex 32)" > fwd.secret                     # 令牌密钥（serve.mjs 读这个文件）
printf 'listen_udp = 127.0.0.1:47995\nlisten_websocket = 127.0.0.1:47996\nidentity_key_file = id.key\nsecret = %s\n' \
  "$(cat fwd.secret)" > fwd.conf
build/release/bin/rcs_forwarder --config fwd.conf
build/release/bin/rcsdk_cli --generate-key K && xxd -p -c 64 K                    # 连接密钥，十六进制填进页面
build/release/bin/rcs_forwarder --sign-token --secret "$(cat fwd.secret)" --identity id.key --room r1 --role publish > share.token
build/release/bin/rcsdk_cli --role share --transport forwarder --address 127.0.0.1 --port 47995 \
  --forwarder-key SHA --token "$(cat share.token)" --key-file K --host-pattern   # 测试图案，不用录屏权限
node examples/web/serve.mjs --port 8737 --forwarder 127.0.0.1:47996 --forwarder-secret-file fwd.secret --forwarder-sha256 SHA
```

在浏览器里打开 `http://127.0.0.1:8737/forwarder.html`，填房间 `r1`、`serve.mjs` 打印的令牌和密钥。期望：状态行几秒内到 `running`，`encoding 0 of 1`、画面约 30 FPS，`forwarder peer 1, opened` 不断增长、`without their key` 和 `refused` 为 0；在页面上点一下后有分享端的声音（`sound … packets` 增长；分享端加 `--audio off` 就没有）；再开第二个页签连同一个房间，它马上有画面（从转发器留着的开始）；停掉再开 `rcs_forwarder`，页面在几秒内重新看上（WebSocket 重开、问了新令牌）。

## 示例

`examples/web/`（测试用，不属于 SDK）：`viewer.html` 是一个观看端页面（每路画面一个画布，拖到画面上的文件发给分享端，分享端发来的文件收进内存列成链接，`?files=opfs` 时收进源私有文件系统），`serve.mjs` 是 node 写的小服务器，带跨源隔离的头提供页面和包，并在 `/relay` 上把页面的 WebSocket 接到 `rcsdk_host_relay` 的一个房间：

```bash
build/release/bin/rcsdk_host_relay --listen 127.0.0.1:47910
build/release/bin/rcsdk_cli --role share --transport host --relay 127.0.0.1:47910 --room r1 --key-file K --allow-control
node examples/web/serve.mjs --port 8737 --relay 127.0.0.1:47910
```

`serve.mjs` 打印页面地址和它这次运行的令牌（`--token` 可以自己给）。在浏览器里打开 `http://127.0.0.1:8737/viewer.html`，填房间、令牌和密钥（十六进制）。分享端加 `--clipboard both`（`--clipboard-host` 时用程序自己的剪贴板，不碰系统的）、`--send-file PATH`、`--receive-dir DIR` 可以试剪贴板和文件。别的机器上的浏览器要 `https`，这个示例还不提供。

`forwarder.html` 是经转发器的观看端页面：`serve.mjs` 加 `--forwarder 地址:端口`（转发器的 `listen_websocket`）、`--forwarder-secret-file 文件`（令牌密钥，`编号:64 个十六进制数字`，和转发器配置的 `secret` 一行相同；从文件读，不放命令行）和 `--forwarder-sha256 十六进制`（转发器身份的 SHA-256）时，它替页面当宿主的服务器：页面把 `serve.mjs` 的令牌和房间 POST 到 `/forwarder-token`，换来这个房间的订阅令牌（`rcsf1`，10 分钟，每个页面一个路由号）、转发器身份的 SHA-256 和一张一次性的刷新票（`refreshToken` 用它换下一个令牌，页面不必留着 `serve.mjs` 的令牌）；`/forwarder` 上的 WebSocket 原样转给转发器的 WebSocket 入口，就像公网上它前面的 TLS 代理（nginx 的 `location /rcs`）。

它默认是安全的，照抄它的宿主要留着这些：只听本机环回（`--listen 地址` 才听别处）；中继是服务器自己的设置（`--relay`），页面不能让它连别的地址；进房间要令牌：页面把令牌放在请求体里 POST 到 `/ticket`，换一张 10 秒内只能开一次 WebSocket 的票；票和 WebSocket 只给这个服务器自己的页面（看 `Origin`），别的网站的页面用不了它；一条 WebSocket 消息最多 16 MiB；转发器的令牌同样只给它自己的页面、只凭它的令牌（或刷新票），`/forwarder` 只转到它被告知的转发器，令牌密钥只从文件读。密钥和令牌都不放进 URL（浏览历史和日志会留下）：页面上手填，或在控制台里 `viewer.connect({ room, token, key })`；密钥只在页面里，服务器从来拿不到（SDK 端到端加密）。`?auto` 只让页面每秒把状态打到控制台。

## 和原生观看端的差别

- 浏览器自己留着的组合键（Cmd+Tab、Ctrl+W 等）到不了页面；Chrome 全屏时的键盘锁（`navigator.keyboard.lock()`）能拿到大部分，由页面决定要不要用。
- 剪贴板和文件由页面在浏览器允许时读写（见上面），不像原生那样监视系统剪贴板。
- 只能用浏览器的默认麦克风；声音要等用户和页面互动后才开始播放（在那之前 `audio` 事件说暂停）。
- 口令身份（`PASSWORD`）的 PBKDF2 在 WASM 里约 0.5～0.9 秒，浏览器里推荐连接密钥或 PIN。
- 包里带剪贴板和文件传输两个官方模块；别的模块（官方的或你自己的）要和 `web/glue.cpp` 一起编进你自己的 WASM。

## 验证到哪里

只在 Mac 上的 Chromium 152（一个内嵌浏览器）里测过，分享端是本机的 `rcsdk_cli`（经 `rcsdk_host_relay` 和 `serve.mjs`）：1080p60 每秒 60 帧，每帧解码约 1.1～1.3 ms，HEVC 4K 每帧约 2.1 ms；页面主线程卡住 3 秒时照常收、解、画 180 帧；声音零丢包；键鼠、文字、粘贴的顺序到达分享端；剪贴板两个方向（文字、HTML、图片）；文件两个方向（目录、空文件、内存、源私有文件系统）和跨运行续传，哈希一致；页面自有通道；12 轮创建、启动、传输中途销毁；连续 4.5 分钟 0 丢包、内存不涨。正式版 Chrome 152 和 Edge 155（无界面方式）1080p60 硬件 H.264 每帧约 1.0～1.2 ms。VP9（硬件）1080p 约 45 FPS、每帧解码 3～9 ms，AV1（硬件）约 45 FPS、每帧约 3 ms，VP8（软件）约 51 FPS、每帧约 3.7 ms（分享端是 `rcsdk_cli` 用 libvpx / libaom 软件编码的测试画面，帧率是编码的上限）；这个 Chromium 报告 VP8 只有软件解码，所以 VP8 要 `acceleration: 'software'`（硬件优先的观看端不报 VP8）。经转发器（同一个内嵌 Chromium，本机环回）：1080p H.264 30 帧/秒，第二个页签 2 秒内从转发器留着的画面开始，转发器重启后两个页签几秒内自己连回来，断开时分享端记成对端关闭。还没测：多路画面（要有多块显示器的分享端）、麦克风（要用户允许）、`navigator.clipboard.write`、触摸、笔、Safari、Firefox、别的机器上的浏览器（要 https）。
