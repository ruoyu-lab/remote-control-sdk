# 15 模块：剪贴板、文件传输、电源、缩略图、文本插入点

剪贴板、文件传输这类功能不在核心里，而是**模块**（`rcsdk/module.h`）：宿主在进程里注册一次，在会话的扩展链里放一项它的设置才启用；不用的模块没有线程、没有消息。官方模块和别人写的模块用同一套接口。

## 用一个模块

```c
#include <rcsdk/clipboard.h>   /* 官方模块的头文件，它包含 rcsdk/module.h */

rcs_register_module(rcs_clipboard_module());          /* 进程里一次，在创建会话之前 */

rcs_clipboard_settings clipboard = RCS_CLIPBOARD_SETTINGS_INIT;
clipboard.flow = RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE;
rcs_module_config use = RCS_MODULE_CONFIG_INIT;
use.module = RCS_CLIPBOARD_MODULE;
use.settings = &clipboard;
use.size = sizeof clipboard;
use.header.next = config.extensions;                  /* 和其他扩展串在一起 */
config.extensions = &use.header;
```

C++：`rcsdk::registerModule(*rcs_clipboard_module()); config.useModule(RCS_CLIPBOARD_MODULE, clipboard);`。

- **链接**：每个官方模块是一个库：`RemoteControl::Clipboard`、`RemoteControl::FileTransfer`、`RemoteControl::Power`、`RemoteControl::Thumbnail`、`RemoteControl::TextCaret`；`RemoteControl::All` 是核心加全部构建了的模块和组件。CMake 选项 `RCSDK_MODULES` 选构建哪些（默认 `all`）。iOS 的 XCFramework 和 Android 的 AAR 里已经包含全部官方模块。链接了库不注册就不生效。
- **注册**（`rcs_register_module`）：进程范围，在用它的会话和助手创建之前；同一个 id 再注册是 `INVALID_ARGUMENT`。`rcs_list_modules` 列出已注册的。
- **启用**（`RCS_EXT_MODULE`、`rcs_module_config`）：一个模块一项，一条链里可以有多个（最多 `RCS_MODULES_MAX` = 32）。设置是模块自己的结构体，SDK 复制 `size` 字节（最多 64 KiB），由模块验证：不合法是 `INVALID_ARGUMENT`、本系统做不到是 `UNSUPPORTED`，原因由模块写。设置里指向的东西（回调的 `user_data`）归宿主，要活得比会话长。`settings` 为 `NULL`、`size` 为 0 时用模块的默认设置。
- **两端都要用**（需要和对端说话的模块）：两端连上时互报各自的模块，模块只在两端都用、版本相同的连接上激活；没激活时 `RCS_EVENT_MODULE` 报告 `RCS_MODULE_INACTIVE` 和原因。
- **运行中改设置**：`rcs_session_update` 带上同一个模块的 `rcs_module_config`。会话用哪些模块在创建时就定了，更新里出现别的模块是 `INVALID_ARGUMENT`：要开关一个模块，就一直用着它、在设置里关掉。
- **事件**：`RCS_EVENT_MODULE`（`rcs_module_event`：模块 id、`kind`、`result`、`message`、`payload`）。`kind` 从 `RCS_MODULE_EVENT_OWN`（256）起是模块自己的，payload 的结构由它的头文件定义。
- **统计**：`rcs_session_get_module_stats`（消息数和字节）；模块自己的计数由它的函数给。
- **模块自己的函数**是普通 C 函数（例如 `rcs_clipboard_send(session, content)`），失败时 `rcs_get_last_call_error` 说明原因。
- **助手在另一个进程时**（无人值守，[16](16-unattended.md)）：那个进程也要注册同一个模块，模块的“机器那一半”（剪贴板的读写、文件的读写、缩略图的拍摄、插入点的读取、电源请求和电源操作）才在那里运行；没注册时会话收到 `RCS_MODULE_NO_MACHINE`，宿主知道为什么剪贴板不工作。

## 剪贴板（`rcsdk.clipboard`）

一端复制的东西，另一端可以粘贴（两种角色都有；头文件 `rcsdk/clipboard.h`）。**默认关闭**：会话不用这个模块就没有剪贴板，用了而 `flow` 为 0 也什么都不传（剪贴板里可能有密码，由宿主打开）。设置是 `rcs_clipboard_settings`：

- **方向**（`flow`）：`RCS_CLIPBOARD_SEND` 本机复制的东西发给对端，`RCS_CLIPBOARD_RECEIVE` 对端复制的东西放到本机。两端各管自己的机器：远程协助里分享端可以只收不发。
- **执行者**（`target`）：`RCS_CLIPBOARD_SYSTEM`（默认）由模块读写系统剪贴板（分享端的是助手所在机器的）；`RCS_CLIPBOARD_HOST` 时模块不碰系统剪贴板：宿主用 `rcs_clipboard_send` 发自己的内容，收到的在 `on_clipboard` 里交给宿主（这时有 `RECEIVE` 就必须有 `on_clipboard`）。
- **格式**（`formats`，`1u << rcs_clipboard_format` 位；0 是本机剪贴板能带的全部格式）：`TEXT`（UTF-8，换行是 LF）、`HTML`（UTF-8 的 HTML 文档）、`RTF`、`IMAGE`（PNG）。每种格式在线上只有一种形式，系统之间的差别（CRLF、CF_HTML 头、位图和 TIFF）两端各自换。文件（`FILES`）要点名，见下面的“复制粘贴文件”。`rcs_clipboard_system_formats()` 是本机系统剪贴板能读写的格式（Windows、macOS、iOS 全部，Android 文字和 HTML）。
- **大小**（`max_bytes`，默认 16 MiB，1 KiB～256 MiB）：一份内容所有格式加起来的上限；装不下的格式按 TEXT、HTML、RTF、IMAGE 的顺序保留前面的。对端收什么、多大：`rcs_clipboard_get_peer`（变了有 `RCS_CLIPBOARD_PEER` 事件）。
- **何时传**：`SYSTEM` 时每隔 `check_interval_ms`（默认 250 ms）看一次系统剪贴板，只传连接期间复制的内容；`send_existing = 1` 时连接开始时本端已有的内容也传过去（通常只在观看端打开）。新内容覆盖还没发出的旧内容；从对端放进来的内容不会再发回去。
- **宿主把关**：`on_clipboard` 先看到每份内容：发出前返回 0 就不发；收到后返回 0 就不放到系统剪贴板。回调不重叠，复制需要的内容后尽快返回。
- **密码**：复制它的程序标成不要保留或分享的内容（密码管理器会这样做）默认留在本机，`send_concealed = 1` 才发；放到对端时带上同样的标记。
- **事件** `RCS_CLIPBOARD_COPY`（payload `rcs_clipboard_event`）：发到了对端，或放到了本机，或没能做到（`BUFFER_TOO_SMALL`、`UNSUPPORTED`、`DEVICE`：系统剪贴板读不了或写不进）。**统计** `rcs_clipboard_get_stats`。
- 系统的限制：iOS 读别的应用复制的内容时可能弹“允许粘贴”；Android 10 起只有前台应用能读剪贴板，图片不收发（[05](05-quickstart-ios.md)、[06](06-quickstart-android.md)）。

**复制粘贴文件**（`RCS_CLIPBOARD_FILES`）：在一端的资源管理器、访达里复制文件或文件夹，到另一端粘贴。**默认关**，要两层都打开：剪贴板的 `formats` 点名 `1u << RCS_CLIPBOARD_FILES`（`formats = 0` 和 `RCS_CLIPBOARD_ALL_FORMATS` 都不含它），并且两端都用文件传输模块，复制的一端有 `RCS_FILE_SEND`、粘贴的一端有 `RCS_FILE_RECEIVE`（缺了的话文件不传，`RCS_CLIPBOARD_COPY` 说明原因）。

- **复制时只传名字**：一份复制里只有顶层条目的名字、种类、大小（`rcs_clipboard_files`），复制一个 50 GB 的文件夹也只传几百字节；对端**粘贴时**才经文件传输取（列目录、读内容）。复制的一端给每个收文件的对端各发布一次（`rcs_file_publish`），本机有了新的复制就撤销（已经开始的粘贴照常做完）。
- **粘贴就是一次文件传输**：`RCS_CLIPBOARD_PASTE`（`rcs_clipboard_paste_event`）在两端说明开始了哪一次（`transfer`），之后用文件传输的事件看进度、结果（`RCS_FILE_DONE`），用 `rcs_file_pause` / `rcs_file_cancel` 暂停、取消。粘贴不成（复制已经被替换、别的程序在粘贴）也有这个事件，`result` 和 `message` 写明。统计：`pastes_here`、`pastes_there`、`pastes_failed`。
- **各系统的粘贴**：Windows 放虚拟文件（`FileGroupDescriptorW` + `FileContents` 流，资源管理器边读边写，有它自己的进度框和取消）；macOS 只给访达一个隐藏的占位文件（`.rcsdk-paste-<编号>`），看到访达把它复制到哪里，就把文件取到那里，再删掉占位（别的程序粘贴拿不到文件，有事件说明；`files_stand_in = 0` 关掉）；iOS 放 `NSItemProvider`，别的应用要时才取。`files_prefetch_bytes`（默认 0）：只有文件、合计不超过这么多字节的复制一到就取进 `files_directory`，取完后作为真文件放上剪贴板，任何程序都能粘贴。`rcs_clipboard_system_files(reason, capacity)` 说本机（这个进程）能读出复制的文件（`SEND`）、能让粘贴拿到对端的文件（`RECEIVE`）与否和原因。
- **宿主自己取**（`target = HOST`，或不想经系统剪贴板）：`on_clipboard` 里收到的 `content->files` 带对端的发布编号，之后任何时候用 `rcs_file_fetch`（`RCS_FILE_FETCH_INTO`，宿主选的目录）取，直到对端有了新的复制。`rcs_clipboard_send` 也能带文件（`content.files`，每项给机器上的路径）。
- **剪切**：复制它的程序标成剪切时，`cut` 为 1，两端都看得到；SDK 从不删除原文件，要不要删由复制那端的宿主决定。
- **安全**：对端只能取本端放到剪贴板上的那些文件（只有编号上线，路径不上线），只在它们还是本端最新的复制时开始取。但要清楚：观看端有远程控制时，它可以在分享端的资源管理器里选中任何文件复制，再在自己那边粘贴，所以**分享端打开 FILES 和文件传输 SEND，就等于有控制的观看端能取走助手账户能读的文件**。不想这样的宿主只打开 RECEIVE，或在 `on_clipboard`（SEND）里按自己的规则拒绝。Windows 上以 LocalSystem 运行的助手（跟随安全桌面）两个方向都不支持文件（原因写明），无人值守要文件剪贴板就让助手以用户身份运行（`RCS_HELPER_USER`）。

```cpp
rcs_clipboard_settings clipboard = RCS_CLIPBOARD_SETTINGS_INIT;
clipboard.flow = RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE;
clipboard.formats = RCS_CLIPBOARD_ALL_FORMATS | (1u << RCS_CLIPBOARD_FILES);   // 文件要点名
rcs_file_transfer_settings files = RCS_FILE_TRANSFER_SETTINGS_INIT;
files.flow = RCS_FILE_SEND | RCS_FILE_RECEIVE;                                 // 文件传输也要允许
config.useModule(RCS_CLIPBOARD_MODULE, clipboard);
config.useModule(RCS_FILE_TRANSFER_MODULE, files);
```

```cpp
#include <rcsdk/clipboard.h>
rcsdk::registerModule(*rcs_clipboard_module());
rcs_clipboard_settings clipboard = RCS_CLIPBOARD_SETTINGS_INIT;
clipboard.flow = RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE;   // 两个方向都同步
clipboard.on_clipboard = [](const rcs_clipboard_content* c, void* app) -> int32_t {
    return static_cast<App*>(app)->userAllowsPaste(*c);       // 先给用户看一眼
};
clipboard.user_data = &app;                                    // 活得比会话久
config.useModule(RCS_CLIPBOARD_MODULE, clipboard);
```

## 电源（`rcsdk.power`）

机器的电源（头文件 `rcsdk/power.h`，两种角色都能用）。三件事，都由宿主打开：会话期间保持唤醒；网络唤醒的积木；观看端请分享端的机器重启、关机、睡眠或注销。取舍见 [API-DESIGN](../API-DESIGN.md#4-决定) 第 42 条。

### 保持唤醒

分享的机器睡着了就连不上，显示器关了就看不到；看着的 iPad 自动锁屏画面就停了。用这个模块的会话在有对端连着时（或整个运行期间）让机器保持唤醒：

- **设置** `rcs_power_settings`：`keep_awake` 是 `RCS_KEEP_AWAKE_OFF`、`SYSTEM`（系统不睡眠，显示器可以关）或 `DISPLAY`（显示器也不关）；`while_running` 为 0 时有对端连着才保持，为 1 时整个运行期间保持（等观看端的时候也不睡，机器才连得上）。设置为 `NULL`、大小为 0 时取默认：有对端连着时 `SYSTEM`，不告诉唤醒信息，不允许任何操作。观看端想让自己的屏幕不熄就设 `DISPLAY`。
- **谁持有**：模块的机器那一半在会话的机器上向系统申请电源请求，理由是 “Remote session in progress”：Windows 的 `PowerCreateRequest`（`powercfg /requests` 里看得到）、macOS 的 `IOPMAssertion`（`pmset -g assertions`）、iOS 关掉应用的空闲计时器（只接受 `DISPLAY`）、Android 的唤醒锁（`SYSTEM` 是 `PARTIAL_WAKE_LOCK`，`DISPLAY` 让屏幕常亮，要 `WAKE_LOCK` 权限）。分享端在它的助手里，观看端在自己的进程里。用户按电源键、合上盖子、电量过低仍然优先。
- **结果**：`RCS_POWER_CHANGED`（payload `rcs_power_status`：设置现在要什么 `wanted`、系统实际持有什么 `held`、两者不同时的原因，以及机器那一半能执行哪些操作 `actions`）。`rcs_power_get_status(session, &status)` 查当前的。系统拒绝时只报告并写日志，运行照常。
- **本机能做什么**：`rcs_power_get_capabilities(&caps)`（`rcs_power_capabilities`）：`keep_awake`（系统接不接受电源请求）、`actions`（本进程能执行的操作）、`interfaces`（能不能列出唤醒网卡）、`send_wake`（能不能发唤醒包）、`message`（缺的是为什么）。

### 网络唤醒

睡着或关着的机器不运行 SDK，只有同一局域网里的另一台设备发 Wake-on-LAN 魔术包才能叫醒它。SDK 给积木，谁去叫、从哪里叫是宿主的：

- **本机的唤醒信息**：`rcs_power_list_interfaces(items, sizeof(rcs_power_interface), capacity, &count)`（两次调用的列表）列出能唤醒本机的网卡：开着、有 IPv4 地址的物理以太网和 Wi-Fi（以太网在前），每项有 `mac`、`link`、`wake`（系统是否允许它唤醒机器：`ENABLED`、`DISABLED`、`UNKNOWN`）、`address`、`prefix_length`、`broadcast`（子网的广播地址）、`name`。Windows 的 `wake` 是设备管理器里“允许此设备唤醒计算机”，macOS 是“唤醒以供网络访问”（全机一个设置）；BIOS 里的设置看不到。SDK 只报告，从不修改系统设置。iOS、Android、浏览器上是 `UNSUPPORTED`（手机和平板不会被网络唤醒）。
- **告诉观看端**：分享端设 `share_wake_info = 1`，观看端就能读到分享端机器（助手所在的机器）的这些网卡：`rcs_power_get_peer(session, &peer)`（`rcs_power_peer`：`wake_info`、`interfaces` 个数、分享端提供的操作）、`rcs_power_get_peer_interfaces(...)`，变了有 `RCS_POWER_PEER` 事件；连接断了还保留到下一次运行开始。MAC 地址能标识一台机器，所以默认不告诉。宿主应把它存进自己的设备记录：对方睡着之后才用得上。
- **发唤醒包**：`rcs_power_send_wake(&wake)`（`rcs_power_wake`：`mac`、`address`、`port`、可选的 SecureOn 口令 `password` / `password_size` 4 或 6）。`address` 填对方子网的广播地址（`broadcast`）最可靠；NULL 是 255.255.255.255，只从系统挑的一个网卡发出。端口默认 9，7 也常用。UDP 可能丢，隔一秒发几次；机器醒没醒要等它重新上线才知道。从哪台设备发由宿主安排：观看端和被控机器在同一个局域网时它自己发；否则宿主让那个局域网里自己的另一台设备（常开的电脑、NAS）发。iOS 发广播要 `com.apple.developer.networking.multicast` 授权；浏览器发不了（`UNSUPPORTED`）。

### 远程电源操作

观看端请分享端的机器重启、关机、睡眠或注销（`rcs_power_action`：`RCS_POWER_RESTART`、`SHUT_DOWN`、`SLEEP`、`SIGN_OUT`），分享端的宿主把关，机器那一半执行。

- **分享端允许**：`actions` 是观看端可以请求的操作（`1u << rcs_power_action` 位，默认 0：全部拒绝）；`on_action(request, user_data)` 对每个请求做决定（`rcs_power_request`：观看端、请求编号、操作、`flags`、它有没有远程控制权；返回非零放行；在读那个观看端连接的线程上调用，要快）。没有回调时：有远程控制权的观看端才行。`action_delay_ms`（默认 3 秒，1～60 秒）：接受后等多久才执行，让答复和事件先发出去。
- **观看端请求**：`rcs_power_request_action(session, action, flags, &request)`，`flags` 可以是 `RCS_POWER_FORCE`（有未保存内容的应用不再拦住重启、关机、注销，内容丢失；不加时应用可能拦住，等机器前的人决定）。立即返回请求编号。先看 `rcs_power_get_peer` 的 `actions`：分享端宿主允许、机器也做得到的操作。
- **两端的事件** `RCS_POWER_ACTION`（payload `rcs_power_action_event`：`request`、`action`、`flags`、`state`、`result`、`delay_ms`、`message`；分享端的带 `viewer_id`）：`REFUSED`（`REFUSED`：宿主不允许；`UNSUPPORTED`：这台机器做不到，原因写明；`INVALID_STATE`：助手不在，或没人登录可注销；`BUSY`：已经有一个在等），`ACCEPTED`（`delay_ms` 后执行），`STARTED`（系统接受了，机器马上要走，这一条可能到不了），`FAILED`（系统当时拒绝了，或在答复前断线，结果未知）。
- **谁执行**：机器那一半，在分享端的助手里（会话进程里或另一个进程里注册了模块的助手），接受后在自己的线程上等延迟再执行；会话停了、观看端断了也照样执行，所以宿主在 `ACCEPTED` 后不要马上退出进程。同一时间只有一个。
- **各系统**：Windows 要进程有关机特权（`SeShutdownPrivilege`：LocalSystem 有，工作站上登录的用户也有）；重启、关机用 `InitiateShutdownW`（完整关机，不是快速启动的混合关机），睡眠用 `SetSuspendState`（现代待机的机器没有能这样进入的睡眠状态：`UNSUPPORTED`），注销的是进程自己的 Windows 会话（不强制时只有那个用户自己的进程能做，LocalSystem 的助手要 `FORCE`）。macOS：控制台用户的进程经 loginwindow 重启、关机、注销（应用会被要求退出），睡眠用 `IOPMSleepSystem`；强制重启、关机要 root（登录窗口上的助手），root 只做强制的。iOS、Android、浏览器做不了（`UNSUPPORTED`）。
- **重启之后**：无人值守的会话靠宿主的服务和看护者回来（Windows 服务自动启动、看护者在登录屏拉起助手、`keep_listening`；macOS 的 LaunchDaemon 和登录窗口的 LaunchAgent），见 [16](16-unattended.md#重启关机与网络唤醒)。观看端断线后重连或由宿主重新开始。

```cpp
#include <rcsdk/power.h>
rcsdk::registerModule(*rcs_power_module());   // 进程里一次，另一个进程里的助手也一样
config.useModule(RCS_POWER_MODULE, nullptr, 0);   // 有对端连着时机器不睡眠（默认设置）

// 分享端：整个运行期间不睡；告诉观看端唤醒信息；允许重启和睡眠，由宿主的回调决定
rcs_power_settings power = RCS_POWER_SETTINGS_INIT;
power.keep_awake = RCS_KEEP_AWAKE_SYSTEM;
power.while_running = 1;
power.share_wake_info = 1;
power.actions = (1u << RCS_POWER_RESTART) | (1u << RCS_POWER_SLEEP);
power.on_action = [](const rcs_power_request* r, void* app) -> int32_t {
    return static_cast<App*>(app)->allowsPowerAction(*r);   // 例如：有控制权、在维护时段内
};
power.user_data = &app;                       // 活得比会话久
config.useModule(RCS_POWER_MODULE, power);    // 同一个 id 再来一次就是替换设置

// 观看端：用模块（默认设置），连上后请求重启，并记下唤醒信息
rcs_power_peer peer = RCS_POWER_PEER_INIT;
rcs_power_get_peer(view.handle(), &peer);
if (peer.actions & (1u << RCS_POWER_RESTART)) {
    uint64_t request = 0;
    rcs_power_request_action(view.handle(), RCS_POWER_RESTART, 0, &request);   // 结果在 RCS_POWER_ACTION 里
}
std::vector<rcs_power_interface> nics(peer.interfaces);
uint32_t count = 0;
rcs_power_get_peer_interfaces(view.handle(), nics.data(), sizeof(rcs_power_interface), uint32_t(nics.size()), &count);
app.remember(nics);                           // 宿主自己的设备记录

// 之后，在同一个局域网里的设备上叫醒它
rcs_power_wake wake = RCS_POWER_WAKE_INIT;
std::memcpy(wake.mac, nics[0].mac, 6);
wake.address = nics[0].broadcast;
rcs_power_send_wake(&wake);
```

## 文件传输（`rcsdk.file-transfer`）

文件和目录从一端到另一端，两种角色都能发、都能收（头文件 `rcsdk/file_transfer.h`；线上格式和安全规则见 [FILE-TRANSFER-DESIGN](../FILE-TRANSFER-DESIGN.md)）。SDK 负责可靠、加密、不抢画面地送到，以及临时文件、改名、冲突、断线续传；收不收、放到哪里、界面怎么做是宿主的。**默认关闭**：`flow` 为 0 时什么都不传，收到的清单要宿主明确接受才写盘。两端都要用它。

设置 `rcs_file_transfer_settings`：

- **方向**（`flow`）：`RCS_FILE_SEND`、`RCS_FILE_RECEIVE`。对端允许什么：`rcs_file_get_peer`（变了有 `RCS_FILE_PEER` 事件）。
- **执行者**（`target`）：`RCS_FILE_SYSTEM`（默认）由模块读写文件（分享端在助手所在的机器、以助手的账户；观看端在本进程）；`RCS_FILE_HOST` 时模块不碰文件：发的条目由宿主给名字、种类、大小、时间，字节由 `read` 回调给出，收到的交给 `write`，`resume` 说以前的运行已经有多少。
- **限制**：`max_items`（默认 10 000）、`max_offer_bytes`（默认不限）；等本端宿主决定的清单的预算 `max_pending_offers` / `max_pending_items` / `max_pending_items_total`（超过的立即以 `BUSY` 拒绝，宿主看不到）；`offer_timeout_ms`（宿主多久不决定就算拒绝，默认 120 秒）；`reconnect_wait_ms`（断线后等对端回来多久，默认 60 秒）；`max_bytes_per_second`（限速，运行中改立即生效）；`keep_partial`（见续传）。

一次传输：

1. 发的一端 `rcs_file_send(session, &offer, &transfer)`：`rcs_file_offer` 里是条目数组（`rcs_file_item`：`SYSTEM` 时是本机的绝对路径，目录连同里面的一切，`name` 可以换个名字）和对哪个观看端（观看端发给分享端是 0），返回传输编号。
2. 收的一端得到 `RCS_FILE_OFFER`（`rcs_file_offer_event`：编号、条目数、合计大小、是不是续传、令牌），`rcs_file_get_items` 列出条目（可以分页）。宿主可以先问用户，然后 `rcs_file_accept(session, transfer, directory, on_conflict)`（`SYSTEM`：已存在的目录的绝对路径；`HOST`：`NULL`）或 `rcs_file_reject(session, transfer, reason)`。
3. 条目一项一项按顺序传：每 250 ms 最多一个 `RCS_FILE_PROGRESS`，每项结束一个 `RCS_FILE_ITEM_DONE`，整次结束一个 `RCS_FILE_DONE`（完成、对方拒绝、取消、失败，`by_peer`、`result`、`message`）。**`RCS_FILE_DONE` 是这次传输的最后一个事件**：收的一端收到它时文件已经就位、临时文件已经删掉。状态变化有 `RCS_FILE_STATE`。任一端都可以 `rcs_file_pause`、`rcs_file_cancel`；`rcs_file_get_transfer` 随时给状态、字节、速度、结果。
4. 这些函数也可以在 `on_event` 里直接调（例如自动接受到一个固定目录）：它们引起的事件在函数返回后由模块自己的线程送出。

- **写文件**（`SYSTEM`）：只接受相对路径，在任何系统上都落在宿主给的目录下面（`..`、绝对路径、盘符、Windows 的保留名都不收）。先写 `名字.<令牌>.rcsdk-part`，完整了设好修改时间再改名；取消或失败时删掉。重名按 `on_conflict`：`OVERWRITE`、`RENAME`（`名字 (1).扩展名`）、`SKIP`、`FAIL`。符号链接不跟随。权限和扩展属性不传：新文件拿这个进程默认的权限（macOS、Android 上看 umask，Windows 上继承目录的访问控制）。能在接收目录里写的其他账户能改其中的东西（带粘滞位的目录里也能抢先占用传输要用的名字），所以收到只有本账户能写的目录。无人值守时助手以 SYSTEM 运行，文件也以 SYSTEM 的权限写：目录要选在合适的地方。原因（条目的、传输的、日志里的）只用条目在线上的相对路径，不写本机路径：对端也会看到它们。
- **重连**：连接断了，传输等待（`RCS_FILE_WAITING`）；同一端在 `reconnect_wait_ms` 内回来就从已经写好的地方继续。
- **跨运行续传**：每次传输有令牌（`rcs_file_transfer_info.token`）。没传完时接收端（`keep_partial = 1`，默认）留下写了一半的文件和一个记录 `.rcsdk-resume-<令牌>`。下次运行，发的一端用同样的条目、把令牌放进 `rcs_file_offer.resume_token` 再发；收的一端看到 `resuming`，接受到同一个目录：已经完成的文件算完成，写了一半的从断处继续。不打算续传了就 `rcs_file_discard(session, directory, token)`。令牌由发送端选，只是名字、不证明是谁：续传会接着上次写的内容写，所以只接受当初那个对端（按宿主自己的身份判断）的续传清单。没说续传的清单碰到目录里同一令牌留下的东西会失败（`INVALID_STATE`），什么都不拿、不删。
- **剩余空间**：接受后先查，同一个卷上同时在收的传输一起算，放不下就整次失败（`DEVICE`，原因写明）。留不留余量由宿主决定（`max_offer_bytes`、选哪个卷、及时丢掉留下的续传文件）。
- **流量**：每次传输最多 4 MiB 在路上，大文件不占内存，对端怎么重发清单或回答都一样；模块的消息和宿主的数据、别的模块轮流发，让路给画面。

**发布和取**（对方要的时候才传）：一端 `rcs_file_publish(session, &publication, id)` 只记下“对端以后可以来取这些条目”，什么都不发，得到 16 字节的发布编号（`RCS_FILE_PUBLICATION_SIZE`）；编号怎么交给对端是宿主的事（剪贴板模块把它放在复制里）。对端 `rcs_file_fetch(session, &request, &transfer)` 时才开始一次传输，两端都没有 `RCS_FILE_OFFER`（发布就是这一端的决定，取就是那一端的），其余事件、暂停、取消和普通传输一样，`rcs_file_transfer_info.origin` 是 `RCS_FILE_ORIGIN_FETCHED`。

- **两种取法**：`RCS_FILE_FETCH_INTO` 像收一次传输一样写进目录（`SYSTEM`）或交给 `write`（`HOST`）；`RCS_FILE_FETCH_READ` 什么都不写，取的一方用 `rcs_file_read(session, transfer, item, offset, size, tag)` 读任意文件的任意一段（每次最多 `RCS_FILE_READ_MAX` = 256 KiB，没回答的合计最多 `RCS_FILE_READS_MAX` = 16 MiB，再多是 `QUEUE_FULL`），用完 `rcs_file_close` 结束：从头读到尾的文件算完成，没读完的算跳过。系统粘贴（资源管理器边读边写）用的就是它。
- **回调**：取的请求带 `on_listed`（条目到了，`rcs_file_get_items` 能列出）、`on_read`（每次读的结果，带 `tag`；源文件变了是 `INVALID_STATE`）、`on_ended`（最后一个）；发布带 `on_fetch`（对端来取了，这一端的传输编号）和 `on_unpublished`（撤销了、为什么）。它们都在模块自己的一个线程上、不在模块的调用里，可以直接调模块的函数。
- **撤销**：宿主 `rcs_file_unpublish`、本端去掉 `RCS_FILE_SEND` 或换了 `target`、对端在 `reconnect_wait_ms` 内没回来、运行结束，发布就撤销，之后的取以 `REJECTED`、`INVALID_STATE` 被拒；已经在进行的取照常做完（`RCS_FILE_UNPUBLISH_END_FETCHES` 一起取消）。
- **限制**：`max_publications`（同时有效的发布，默认 256，再发布是 `BUSY`）、`max_fetches`（一个对端同时进行的取，默认 4，再取以 `BUSY` 被拒）；目录在每次取时才列出，条目数、字节数和单条消息的上限和普通传输一样（大目录把两端的 `max_items` 调到 100 000）。
- **安全**：对端只能用编号取它被允许的那一份，机器上的路径从不上线；但它取的是“取的时候”条目里的东西：发布了一个目录，对端就能读到这一端的账户（无人值守时是助手的账户）在里面能读的一切。

```cpp
#include <rcsdk/file_transfer.h>
rcsdk::registerModule(*rcs_file_transfer_module());   // 进程里一次，助手进程也一样
rcs_file_transfer_settings files = RCS_FILE_TRANSFER_SETTINGS_INIT;
files.flow = RCS_FILE_SEND | RCS_FILE_RECEIVE;
config.useModule(RCS_FILE_TRANSFER_MODULE, files);
// 发：一个文件和一个目录，给观看端 1（观看端发给分享端是 0）
rcs_file_item items[2] = {};
items[0].path = "/Users/me/report.pdf";
items[1].path = "/Users/me/photos";
rcs_file_offer offer = RCS_FILE_OFFER_INIT;
offer.viewer_id = 1;
offer.count = 2;
offer.items = items;
uint64_t transfer = 0;
rcs_file_send(session.handle(), &offer, &transfer);
// 收：清单来了，问过用户后接受到下载目录，重名就改名
callbacks.onEvent = [&](const rcs_event& e) {
    if (e.type == RCS_EVENT_MODULE && std::strcmp(e.module.module, RCS_FILE_TRANSFER_MODULE) == 0 &&
        e.module.kind == RCS_FILE_OFFER) {
        const auto& o = *static_cast<const rcs_file_offer_event*>(e.module.payload);
        app.askUser(o.transfer, o.items, o.total_bytes);   // 之后 rcs_file_accept(..., "/Users/me/Downloads", RCS_FILE_CONFLICT_RENAME)
    }
};
```

## 缩略图（`rcsdk.thumbnail`）

在分享会话采集的那台机器上（助手在另一个进程、另一个账户时就在那里）拍一张一次采集会共享的画面，给“选择要共享的东西”的界面用。本机的不需要模块：`rcs_capture_picture`（[07](07-sharing.md#共享什么rcs_capture_config)）。

- 只有分享端用，不带设置；会话和助手的进程都注册它。
- `rcs_thumbnail_capture(session, &what, max_width, max_height, &picture)`：`what` 是单独的一个 `RCS_EXT_CAPTURE` 扩展；结果和检查同 `rcs_capture_picture`，但针对助手那台机器。它等答复：不要在界面线程或会话的回调里调用。会话没在运行、没有助手、助手进程没注册时 `INVALID_STATE`，五秒没答复 `TIMEOUT`。

## 文本插入点（`rcsdk.text-caret`）

被控的机器上现在打字会落到哪里（有键盘焦点的文本框的插入点），让观看端的输入法在那里显示组字和候选（头文件 `rcsdk/text_caret.h`）。两端都用这个模块：

- 设置 `rcs_text_caret_settings`：观看端的 `ask`（默认 1：有控制权时要），分享端的 `interval_ms`（默认 100 ms，多久看一次；0 是不看，观看端得知它不告诉）。
- 观看端有控制权时，分享端在它的机器上看，变了才告诉观看端：`RCS_EVENT_MODULE`，`kind` `RCS_TEXT_CARET_CHANGED`，payload `rcs_text_caret`（可见、位置和大小，和输入同一个 0..65535 的坐标空间；用 `framePosition` 和 `ViewGeometry::point` 换算到窗口）。分享端告不告诉：`RCS_TEXT_CARET_TELLS` 事件和 `rcs_text_caret_get_peer`。
- 读插入点：Windows 用有键盘焦点的线程的系统光标，没有时用 UI Automation（Chromium、Electron 一类程序被问到时会打开自己的无障碍支持，有些开销，所以只在观看端要、而且有控制权时才看）；macOS 用辅助功能。`rcs_text_caret_available()` 说本机能不能看。iOS、Android 上报告不可用。
- 宿主自己执行输入（`RCS_INPUT_HOST`）时，系统的插入点可能和输入落到哪里无关，这种宿主不用这个模块。

## 写自己的模块

一个模块就是一张函数表（`rcs_module_definition`）加一个 id（`com.example.whiteboard` 这样倒写自己的域名；`rcsdk.` 开头的留给官方模块）。核心给模块的是：两端互报的模块清单和问候、按观看端的可靠和不可靠消息（`rcs_module_send`）、在被控机器上（助手里）跑的“机器那一半”（`rcs_module_send_machine`）、自己的事件（`rcs_module_emit`）和统计；调用不重叠，模块状态不用为 SDK 的回调加锁。完整的接口和规则见 [MODULES](../MODULES.md)；`examples/pointer/` 是一个不到 100 行、只用公开头文件写的例子（观看端告诉分享端它指着屏幕的哪里）。
