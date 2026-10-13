# 16 无人值守、助手进程与看护者

目标：机器开着就能连，没人登录、锁屏、登录屏、UAC 弹窗下都能看、能操作；用户注销、切换用户、程序崩溃时自动恢复。SDK 给宿主几块积木，服务怎么装、怎么布置由宿主决定。

## 助手：采集放在另一个进程

分享端分两半：会话持有连接（身份、协商、码率、数据通道、重连、事件），**助手**做和这台机器有关的事（采集和编码画面、采集声音、跟随光标和显示器、执行远程输入）。默认助手就在会话的进程里，宿主什么都不用做。需要时把助手放进另一个进程：

- **系统服务 + 用户会话里的助手**：服务持有连接，可以开机就运行、没有桌面也行；助手跑在用户的登录会话里采集屏幕。用户注销、切换用户、助手崩溃时，观看端的连接不断。
- **权限分开**：只让有屏幕录制权限的小程序做采集，持有网络连接的进程不需要这个权限。

两边都在 `rcsdk/agent.h`（C++ `rcsdk/agent.hpp`）：

- **会话一侧**挂 `rcs_agent_link_config`（`RCS_EXT_AGENT_LINK`，在一次运行中固定）：`endpoint`（Windows：命名管道的名字；其他系统：Unix 套接字文件的绝对路径，所在目录必须属于会话的账户或 root、别的账户改不了其中的条目：组和其他人都不可写，除非像 `/tmp` 那样带粘滞位；Android 上可以是应用自己的组可写。否则别的账户可以在套接字或锁文件的位置放上自己的文件或链接，这次运行以 `RCS_ERROR_NETWORK` 失败并说明原因）、两边共有的 32 字节 `key`、`access`（谁能打开端点：`RCS_AGENT_ACCESS_PRIVILEGED` 是会话所在的用户，Windows 上加 SYSTEM 和提权的管理员；`ANY_USER` 是本机所有用户），以及可选的 `on_agent_attach(agent, user_data)`：通过密钥握手的助手要连上时问宿主（看到的进程号、Windows 会话号、账户和程序路径来自系统，不是助手自报），非零接受、0 拒绝。按 `rcs_agent_info.program` 判断是哪个程序，不要拿 `process_id` 再去查：那时这个进程号可能已经是别的进程了。macOS 上程序路径取自系统给这条连接的审计令牌（audit token），它指的是最后用这条连接的进程、现在运行的程序；会话在助手说完握手后再核对一次，连上之后把连接交给别的进程、或者换成别的程序（exec）的，在问宿主之前就被拒绝（`RCS_ERROR_AUTHENTICATION`）。Windows 上路径取自连接时打开的进程。Linux、Android、iOS 上系统说不准，`program` 是空的。C++：`config.use(rcsdk::agentLink(endpoint, key, access, &gate))`，`gate` 是宿主实现的 `rcsdk::AgentGate`。
- **助手一侧**用 `rcs_agent_config`（`endpoint`、`key`、`access`：它理哪些会话，默认 `PRIVILEGED` 只理以自己的账户、LocalSystem / root 或提权的管理员运行的会话；`reconnect`、`reconnect_interval_ms`；扩展链里只收 `RCS_EXT_LOG` 和 `RCS_EXT_HOST_ENCODER`）和 `rcs_agent_create` / `start` / `stop` / `wait` / `destroy`，状态由 `rcs_agent_callbacks.on_event` 报告（`RCS_AGENT_EVENT_STATE`）。C++：`rcsdk::Agent(rcsdk::AgentConfig(endpoint, key), onEvent)`。

要点：

- **设置都在会话一侧**：编码格式、画质、显示器、声音、控制照常写在会话的扩展里，运行中用 `rcs_session_update` 改；助手没有这类配置。
- **检查时机**：哪些编码格式可用、有哪些显示器、有没有屏幕录制和输入权限，要看助手所在的机器，所以这些检查在助手连上时做，做不到时这次运行以原因结束。`rcs_session_get_agent_info` 和 `RCS_EVENT_AGENT`（`rcs_agent_info`）给出助手的进程、Windows 会话号和账户（都由系统报告）、能力和权限，离开后还有原因（`result`、`message`）；`rcs_session_list_displays` 列出助手那台机器的显示器（`rcs_list_displays` 只描述调用它的进程）。
- **助手来去**：第一个助手连上前运行停在 `STARTING`，不开端口。之后助手离开时运行继续：观看端保持连接（新观看端也照样能连），画面暂停（`RCS_EVENT_CAPTURE` 的 `AGENT`），控制权暂停，声音暂停；下一个助手连上后从关键帧继续。等多久由宿主决定，SDK 不会自己结束运行。只有助手在会话进程里时，它的错误才结束运行。
- **端点被占**：别的进程先占了端点的名字，这次运行以 `RCS_ERROR_NETWORK` 失败，原因说明是谁占着：Unix 上是那个套接字或锁文件所属的账户，Windows 上是占着管道名的进程和账户（看得到时）。助手按 `access` 不理占名者，但会话启动不了：端点名要让别的账户猜不到或占不到，例如放在只有会话账户能改的目录里，Windows 上用每次运行随机生成、经看护者交给助手的名字。
- **替换**：被接受的新助手替换旧的，旧的被告知后结束（`STOPPED`，`RCS_ERROR_REPLACED`），不再重连，要不要重新启动它由宿主决定。连上就不说话、密钥不对、版本不对的连接在各自的线程上检查，占不住端点。
- **密钥不落盘**：服务以 SYSTEM 运行、助手由看护者启动时，服务每次启动生成一个链接密钥，交给会话（`rcs_agent_link_config.key`）和看护者（`rcs_supervisor_config.helper_key`），助手用 `rcs_helper_get_key` 取（经继承的管道，不在命令行或环境里）。
- **不能组合**：画面来源 `RCS_CAPTURE_HOST` 和本地预览（`on_frame` / `on_gpu_frame`）不能和助手进程一起用。声音来源 `RCS_AUDIO_HOST`、输入目标 `RCS_INPUT_HOST` 不经过助手，照常可用。`exclude_current_process`（画面和声音）指的是助手进程。
- **模块**：助手进程也要注册会话用的模块，模块的机器那一半才在那里跑（[15](15-modules.md)）。剪贴板的文件（`RCS_CLIPBOARD_FILES`）在 Windows 上以 LocalSystem 运行的助手（`RCS_HELPER_SYSTEM`，跟随安全桌面）里两个方向都不支持：以 SYSTEM 去读用户放在剪贴板上的路径是越权，资源管理器也调不了 SYSTEM 进程里的数据对象。要在无人值守时用文件剪贴板，看护者用 `RCS_HELPER_USER`（代价是不能跟随安全桌面）；macOS 的助手进程要跑主运行循环（`CFRunLoopRun()`），访达的粘贴才问得到它。`rcs_clipboard_system_files` 在助手进程里调，说明那里能做什么。
- 链接上只有编码后的画面、PCM 声音、光标和少量控制消息，GPU 纹理不出助手进程；实测按键到画面的延迟和助手在本进程时在同一范围。两边必须是同一个 SDK 版本：版本不同时助手以 `FAILED` / `UNSUPPORTED` 结束、不再重试。

```cpp
#include <rcsdk/agent.hpp>
#include <rcsdk/supervisor.hpp>

// 服务进程（会话）。链接密钥每次启动新生成，经看护者交给被看护的助手（rcs_helper_get_key），不落盘
const auto linkKey = rcsdk::generateConnectionKey();
rcsdk::Supervisor supervisor("\"C:\\Program Files\\MyApp\\myapp.exe\" --agent", RCS_HELPER_USER, onSupervisorEvent,
                             linkKey);
// 只要看护者刚启动的那个助手（进程信息由系统给出，不是助手自报）
class SupervisedOnly final : public rcsdk::AgentGate {
public:
    explicit SupervisedOnly(rcsdk::Supervisor& supervisor) : supervisor_(supervisor) {}
    bool attach(const rcs_agent_info& agent) override {
        const auto now = supervisor_.status();
        return now.helper_running && now.process_id == agent.process_id;
    }

private:
    rcsdk::Supervisor& supervisor_;
};
SupervisedOnly gate(supervisor);   // 活得比会话久
auto config = rcsdk::SessionConfig(RCS_ROLE_SHARE).setAddress("0.0.0.0", 47990).setKey(key)
                  .use(rcsdk::agentLink("myapp-agent", linkKey, RCS_AGENT_ACCESS_ANY_USER, &gate))   // 助手以普通用户运行
                  .setControl(true);
rcsdk::Session session(config, std::move(callbacks));
session.start();    // 第一个助手连上之前停在 STARTING
supervisor.start();

// 用户会话里的助手进程（看护者启动）
rcsdk::Agent agent(rcsdk::AgentConfig("myapp-agent", rcsdk::helperKey()),
                   [](const rcs_agent_event& e) { /* 记日志：e.state.value、e.state.result */ });
agent.start();   // 找不到会话就每秒再试，链接断了也一样
agent.wait();    // 被另一个助手替换（RCS_ERROR_REPLACED）、版本不同（UNSUPPORTED）或 stop 时返回
```

## Windows

难处有两个：Windows 把服务放在没有桌面的会话 0，而采集屏幕的进程必须在用户会话里；登录屏、锁屏、UAC 又都显示在只有 LocalSystem 才能打开的安全桌面上。积木：

- `rcs_capture_config.secure_desktop`：采集和远程输入跟随当前接收输入的桌面（默认关）。
- **看护者** `rcs_supervisor_*`（组件，头文件 `rcsdk/supervisor.h`，C++ `rcsdk/supervisor.hpp`，库 `RemoteControl::Supervisor`）：在宿主的服务里运行，在活动的控制台会话里（或宿主选的会话里）用 SYSTEM（或登录用户）的身份启动并看住宿主指定的进程。被看护进程那一边（`rcs_helper_wait_for_stop`、`rcs_helper_get_key`，C++ `rcsdk::waitForStopRequest`、`rcsdk::helperKey`）在核心里，被看护进程不用链接这个库。
- `RCS_EVENT_DESKTOP`（用户桌面还是安全桌面）、`RCS_INPUT_SECURE_ATTENTION`（Ctrl+Alt+Del）、电源模块（保持唤醒、远程重启和网络唤醒，见下面[重启、关机与网络唤醒](#重启关机与网络唤醒)）、`RCS_STOP_RESTART`。

**两种布置**（接口相同，宿主选）：

| | 方案 A：被看护的进程就是分享会话 | 方案 B：会话留在服务里，被看护的进程是它的助手 |
| --- | --- | --- |
| 服务里跑 | 只有看护者 | 看护者和分享会话（`rcs_agent_link_config`） |
| 看护者启动的进程 | 整个分享会话（网络、身份、采集） | 助手（`rcs_agent_*`），只做采集、编码、输入 |
| 开机，还没人登录 | 看护者在控制台会话（显示登录屏）里拉起进程；观看端能连上，看到登录屏 | 服务里的会话一直在监听；看护者拉起助手后画面出来 |
| 锁屏 / 解锁、UAC、Ctrl+Alt+Del 屏 | 同一个会话，只是输入桌面切换：采集和输入附着到新桌面，观看端连接不断 | 同左 |
| 注销、切换用户 | 进程用 `RCS_STOP_RESTART` 停下，在新会话里重启；观看端立即重连，画面断几秒 | 观看端的连接不断，画面暂停几秒后从关键帧继续 |
| 进程崩溃 | 看护者重启它；观看端 5 秒后发现并重连 | 看护者重启它；观看端的连接不断 |
| 代价 | 简单；每次重启都有一次重连 | 多一个进程和一条本机链接；网络由服务持有 |

```cpp
#include <rcsdk/agent.hpp>        // rcsdk::waitForStopRequest
#include <rcsdk/power.h>
#include <rcsdk/supervisor.hpp>

// 服务进程（LocalSystem，会话 0）：控制台会话里始终有一个被看护的进程
rcsdk::Supervisor supervisor("\"C:\\Program Files\\MyApp\\myapp.exe\" --helper", RCS_HELPER_SYSTEM,
                             [](const rcs_supervisor_event& e) { /* 记日志、通知宿主的界面 */ });
supervisor.start();                       // 服务停止时 supervisor.stop()

// 被看护的进程（方案 A：它就是分享会话；方案 B 里这里是 rcsdk::Agent，它也注册保持唤醒模块）
rcsdk::registerModule(*rcs_power_module());
auto config = rcsdk::SessionConfig(RCS_ROLE_SHARE).setAddress("0.0.0.0", 47990).setKey(key)
                  .setControl(true)
                  .setSecureDesktop(true)                    // 登录屏、锁屏、UAC 也采集、也能操作
                  .useModule(RCS_POWER_MODULE, nullptr, 0);  // 有观看端时机器不睡眠
rcsdk::Session session(config, std::move(callbacks));
session.start();
rcsdk::waitForStopRequest();              // 服务停止，或控制台换了会话
session.stop(RCS_STOP_RESTART);           // 观看端知道这边会回来
```

要点：

- **跟随安全桌面**：采集线程和输入线程附着到当前接收输入的桌面，桌面切换后重建桌面复制；编码器和 D3D 设备继续用，画面从下一帧继续。需要 LocalSystem 身份、运行在用户会话里（`rcs_capabilities.secure_desktop`）；做不到时这次运行以 `UNSUPPORTED` 结束并说明，不会悄悄退回成暂停。不打开时，锁屏和 UAC 让采集和控制权暂停：每秒一次的检查暂停控制权，在那之前，每个输入事件执行前也看一眼接收输入的是不是用户自己的桌面，不是就不执行（`PERMISSION_REQUIRED`），桌面切换和下一次检查之间的输入也送不到安全桌面。打开后远端看得见、也操作得了登录屏，包括输入密码（密码框是掩码）：只在信任观看端时打开。
- **桌面状态**：`RCS_EVENT_DESKTOP` 和 `rcs_session_get_desktop_info` 说明机器现在显示用户桌面（`RCS_DESKTOP_USER`）还是安全桌面（`RCS_DESKTOP_SECURE`），以及有没有人登录（`user_logged_on`：有，是锁屏或弹窗；没有，是登录屏）。Windows 把登录屏、锁屏和 UAC 放在同一个桌面上，SDK 分不出它们。
- **Ctrl+Alt+Del**：观看端发 `RCS_INPUT_SECURE_ATTENTION`，分享端的 `on_input` 照常先看到它。有控制权的观看端都能发（和任何按键一样）：不想让观看端按的宿主在 `on_input` 里丢掉它，或用 `host_input_types` 把这一类交给自己逐个决定。Windows 的 `SendSAS` 只有会话 0 里的服务能调用，且要策略 `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System\SoftwareSASGeneration` 为 1（服务）或 3（服务和辅助功能程序）；SDK 只读这条策略，从不修改。方案 B 里持有会话的服务自己发；方案 A 里被看护的进程经看护者交给它的管道（继承的句柄，管道只连着它）请看护者发，发到它自己令牌的会话。结果在 `rcs_input_stats.last_result`：`PERMISSION_REQUIRED` 是策略不允许，`UNSUPPORTED` 是本进程做不到。条件满足时 `rcs_capabilities.secure_attention` 为 1。
- **看护者**（`rcs_supervisor_config`）：`command_line` 里的程序要写本机盘上的完整路径和扩展名，带空格时加引号（否则 `INVALID_ARGUMENT`；网络共享上的程序，`\\server\share\...`、`\\?\UNC\...`、映射到共享的盘符，也是 `INVALID_ARGUMENT`：能写那个共享的人就能以 SYSTEM 运行程序）；它以看护者的权限运行，宿主要让它只有管理员能改。`account`：`RCS_HELPER_SYSTEM`（复制服务的 SYSTEM 令牌，登录屏上也能运行、能跟随安全桌面）或 `RCS_HELPER_USER`（已登录用户的令牌，没人登录时报告 `RCS_SUPERVISOR_NO_SESSION`）；两种都要求看护者本身是 LocalSystem（否则 `PERMISSION_REQUIRED`）。被看护的进程退出后等 `restart_delay_ms`（默认 2 秒）重启，连续在 30 秒内退出才逐次翻倍，不超过 `restart_delay_max_ms`；要求它结束时先给 `stop_grace_ms`（默认 3 秒）。事件 `HELPER_STARTED` / `HELPER_EXITED` / `HELPER_FAILED` / `NO_SESSION` / `FAILED`；`rcs_supervisor_get_status` 查当前。每个被看护的进程放在自己的“关闭即结束”的作业对象里（一个作业只能装同一个 Windows 会话的进程：注销后重新登录、换用户、远程桌面登录都换了会话），服务怎么退出它都不会留下，它结束时它开的进程也一起结束。非 Windows 上 `rcs_supervisor_create` 返回 `UNSUPPORTED`。
- **选择会话**（远程桌面、终端服务器、云桌面）：`rcs_list_os_sessions`（C++ `rcsdk::osSessions()`）列出这台机器的会话（`rcs_os_session_info`：编号、状态 `ACTIVE` / `CONNECTED` / `DISCONNECTED` / `NO_DESKTOP` / `OTHER`、是不是控制台、是不是远程、登录的账户、客户端的名字和地址）。把 `os_session` 设成其中一个编号，看护者就在那个会话里运行被看护进程：会话是 `ACTIVE` 或 `CONNECTED` 时运行，客户端断开、注销后立即要求它结束，重连后立即重新启动。运行中用 `rcs_supervisor_update` 换会话（命令行、账户、`helper_key` 在看护者的一生里不变）。客户端的地址是客户端自己报的，只能拿来显示，不能当作身份。真实的远程桌面会话还没有验证过。
- **远程桌面会话里的采集**只用桌面复制；复制在远程桌面会话里失败时采集以 `RCS_ERROR_CAPTURE` 结束，原因写明是哪个会话、哪个 `HRESULT`（Windows 10 上远程桌面客户端断开再重连后复制可能失败）。看护者守着远程桌面会话时，客户端一断开就结束被看护进程，重连后启动新的。

**宿主要做的事**（安装器清单；SDK 不做，也不会替宿主做）：

- 用管理员权限注册服务：LocalSystem、自动启动、失败后自动重启；看护者的命令行指向只有管理员能改的程序，程序所在的目录也一样（例如 Program Files 下），否则别的用户换掉它或在旁边放 DLL，就以 SYSTEM 身份运行了。
- 存放密钥和日志的目录由服务自己创建，访问控制只给 SYSTEM 和管理员；已经存在时先核对所有者和访问控制，不对就拒绝（普通用户可能在 `%ProgramData%` 下抢先建好目录、放进自己的密钥）。读密钥文件前同样核对。链接密钥用 `helper_key` 交给助手，不用存。
- 要 Ctrl+Alt+Del 时设置上面那条系统策略。
- 防火墙：给分享进程的可执行文件放行入站，或者只用 P2P / 中继，这样只有出站连接。
- 代码签名：没签名的服务在很多企业环境里会被拦。
- 凭据：设备私钥或口令放在只有 SYSTEM 能读的地方（机器范围的 DPAPI，或设了 ACL 的文件 / 注册表项）；普通用户的界面程序和服务之间怎么通信由宿主设计。
- 访问控制：无人值守时机器前没有人点“允许”，要靠设备密钥（`RCS_SECRET_NONE` + 信任列表）或长随机口令（[10](10-security.md)）。
- 提示和同意：在被控机器上显示有人在连接，保存授权记录（SDK 提供 `RCS_EVENT_PEER`、`RCS_EVENT_CONTROL`、`RCS_EVENT_DESKTOP` 这些事件，但没有界面）。

**示例**：`examples/unattended`（`rcsdk_unattended`）把这些串成一个能装的服务：`install --mode a|b`、`uninstall`、`service`、`session`、`agent`、`sessions`（[examples/README](../../examples/README.md)）。它只是测试程序。**验证状态**：看护者、跟随桌面（用新建的桌面代替 Winlogon 测试）、保持唤醒、停止通知和 Ctrl+Alt+Del 的中转在实机上验证过；真实的锁屏、登录屏、UAC、`SendSAS`、注销 / 切换用户、开机后登录前连上还没有在真机上试过（[ROADMAP](../ROADMAP.md)）。

## macOS

macOS 上不需要看护者：launchd 就是。做法相当于 Windows 的方案 B：

| | 由谁启动 | 身份 | 做什么 | 要不要系统授权 |
| --- | --- | --- | --- | --- |
| 会话 | LaunchDaemon（`/Library/LaunchDaemons`），开机就运行、退出就重启 | root | 分享会话：网络、密钥、观看端（`rcs_agent_link_config`，Unix 套接字） | 都不要 |
| 助手 | LaunchAgent（`/Library/LaunchAgents`），`LimitLoadToSessionType` 为 `LoginWindow` 和 `Aqua` | 登录窗口上是 root，用户登录后是这个用户 | `rcs_agent_*`：采集、编码、声音、输入 | “屏幕录制”，远程输入还要“辅助功能” |

- **launchd 负责换助手**：登录窗口出现时它在登录窗口里启动一个助手，用户登录后在用户的会话里启动一个，注销时结束用户的那个；会话和观看端的连接一直不断，画面暂停（`AGENT`）几秒后从关键帧继续。快速切换用户时每个登录着的用户各有一个助手，会话只能连一个：宿主在 `on_agent_attach` 里只认控制台前的那个（`SCDynamicStoreCopyConsoleUser`；登录窗口上是 root）。
- **会话不能自己采集**：LaunchDaemon 不在任何窗口服务器会话里，ScreenCaptureKit 列不出显示器，`rcs_desktop_info` 是 `UNKNOWN`。采集只能在 LaunchAgent 里。
- **桌面状态**：助手按自己的窗口服务器会话报告：登录窗口是 `SECURE`、`user_logged_on` 为 0；锁屏是 `SECURE`、为 1；用户桌面是 `USER`。锁屏时采集照旧暂停。
- **系统授权是关键**：助手要“屏幕录制”（远程输入还要“辅助功能”），macOS 按程序的代码签名记录：程序要用 Developer ID 固定签名。**屏幕录制必须有人在这台 Mac 前批准一次**：MDM 的 PPPC 描述文件对屏幕录制只能“拒绝”或“允许标准用户自己批准”，不能直接允许；辅助功能可以由 MDM 直接允许。SDK 不绕过这些。没有屏幕录制授权的助手附着上来，会话检查它的机器时以 `PERMISSION_REQUIRED` 结束这次运行；宿主可以在 `on_agent_attach` 里先看 `rcs_agent_info.screen_permission`，拒绝它，让会话继续等。
- **会话等到助手才监听**：没有助手附着时会话不打开网络端口；所以只要还没有一个有授权的助手，这台 Mac 就连不上，宿主应在安装时就引导用户批准。
- **登录窗口**的画面能不能采到、登录窗口上 root 的助手的授权怎么来，还没有在真机上验证。密码框开着安全输入，合成的按键进不去。**FileVault** 的开机前解锁在固件阶段，任何远程方案都到不了。
- **宿主要做的事**：把程序装在只有 root 能改的目录里（例如 `/Library/<宿主>/`）；写两份 plist 并用 `launchctl bootstrap` 加载；连接密钥和链接密钥都放在只有 root 能读的地方，助手以每个登录用户的身份运行，就给控制台前的用户一份只属于他、只有他能读的链接密钥副本，再在 `on_agent_attach` 里按 `program` 和 `account` 核对助手；端点放在只有 root 能改的目录里（`/var/run` 组 `daemon` 可写，不行）；防火墙放行会话的入站，或只用 P2P / 中继；在被控 Mac 上提示有人在连接。
- **示例**：`examples/unattended` 在 macOS 上的版本：`sudo rcsdk_unattended install` 检查程序和目录只有 root 能改、生成只有 root 能读的密钥、写好并加载两份 plist；端点默认是这个目录里的 `agent.sock`；会话给控制台前的用户写一份他自己的链接密钥副本（`link-<uid>.key`，0600），只接受“这个程序（按系统报告的路径）、控制台前的账户、有屏幕录制授权”的助手。

## 重启、关机与网络唤醒

无人值守的机器睡着或关了就连不上，远程支持又常常要“重启一下再连”。电源模块（[15](15-modules.md#电源rcsdkpower)）给两样积木，重启后怎么回来靠宿主已有的布置：

- **远程电源操作**：分享端的设置 `actions` 允许观看端请求哪些（重启、关机、睡眠、注销，默认都不允许），`on_action` 逐个决定（没有回调时：有控制权的观看端才行）。执行的是助手里模块的机器那一半：Windows 两种方案里都是控制台会话里被看护的进程（SYSTEM 或登录用户，都有关机特权），macOS 是 LaunchAgent 的助手（登录窗口上是 root，只做强制的；用户会话里经 loginwindow，不强制）。接受后等 `action_delay_ms`（默认 3 秒）再执行，答复先发出去；会话这时停了也照样执行。
- **网络唤醒**：`share_wake_info = 1` 时观看端拿到这台机器的网卡（MAC、子网广播地址、系统是否允许唤醒），宿主存进自己的设备记录；机器睡着或关机后，宿主让同一局域网里的设备调 `rcs_power_send_wake`。SDK 只报告唤醒设置，从不修改。

**重启后回来，宿主要配好的**（SDK 不替宿主做）：

- **Windows**：服务设成自动启动（失败后自动重启）；看护者用 `RCS_HELPER_SYSTEM`，开机后没人登录时也在控制台会话（登录屏）里拉起被看护进程（用 `RCS_HELPER_USER` 时要等有人登录）；`secure_desktop` 打开才看得到、操作得了登录屏；分享会话 `keep_listening`（默认）。方案 A 里重启前不需要 `RCS_STOP_RESTART`：机器关机时连接断开，观看端照常重连。观看端的重连窗口（`reconnect_window_ms`，默认 30 秒）通常比重启短：观看端宿主在 `ACCEPTED` 之后断线时，自己隔一会儿重新 `rcs_session_start`。BitLocker 要开机 PIN 的机器停在开机前的输入界面，远程到不了（只用 TPM 的不受影响）。网络唤醒要在 BIOS 和网卡属性（“唤醒魔术包”“允许此设备唤醒计算机”）里打开；从关机状态唤醒常常要关掉“快速启动”。
- **macOS**：LaunchDaemon（`RunAtLoad`、`KeepAlive`）开机就运行会话，登录窗口的 LaunchAgent 拉起 root 的助手，所以重启后停在登录窗口时也能连上（登录窗口的画面能不能采到还没在真机上验证）。**FileVault** 打开时，重启后停在固件阶段的解锁界面，没有网络：无人值守的重启要关掉 FileVault，或由宿主自己用 `fdesetup authrestart`（要管理员凭据或恢复密钥）。“唤醒以供网络访问”在系统设置的电池 / 节能里，有线网口最可靠。
- **睡眠**：醒来后进程还在，观看端重连即可。`keep_awake` 只阻止机器自己睡，观看端请求的 `SLEEP` 照样睡。

**示例**：`rcsdk_unattended install ... --power-actions all --share-wake-info`（Windows；macOS 是 `--share-wake-info yes`）让有控制权的观看端可以请求，日志里记下每一步；`rcsdk_cli --role view ... --power-ask restart` 请求，`rcsdk_cli --wake MAC --wake-address 广播地址` 唤醒，`rcsdk_cli --wake-info` 列出本机的网卡。**验证状态**：Mac 上的单元测试和会话场景（动作交给测试替身）通过，真实的重启、关机、睡眠、注销和唤醒还没有在实机上做过。

## 安全模型

- 助手以 SYSTEM 身份运行时，它的任何漏洞都等于 SYSTEM 权限；方案 A 里网络握手、解析和采集都在 SYSTEM 里。网络握手在任何其他解析之前完成；能用用户方式的助手就别用 SYSTEM。
- **本机链接分几层**，每层回答一个问题：谁能打开端点（系统的访问控制，`access`）；对方到底是哪个进程、哪个账户（系统说的，不是对方说的）；会话认不认这个助手（密钥握手，然后 `on_agent_attach`）；助手认不认这个会话（密钥握手之前先看系统报告的服务端进程，别的用户抢先占了端点名字也骗不到它）。链接的密钥派生时绑定了“链接”这个用途，网络上的对端不会被当成链接的对端。
- 被看护进程的环境变量只在父进程正是那个看护者时才被采用，读过后从环境里删掉；Ctrl+Alt+Del 的中转管道每个被看护进程一条，启动前就连着它继承的那一头：别的进程（同账户的也一样）打不开、占不住，不能替它请求，也不能让它的请求发不出去。
- SDK 不修改任何系统设置（注册表策略、防火墙、服务配置），不满足条件时只报告缺什么；不提供任何隐藏运行、规避检测的东西。
