# 安全评估报告

> 本报告是对 SDK 做的一次防御性安全评估：找出弱点，以便修复。报告区分“已用测试验证”“已通过代码审阅确认”“本轮未充分覆盖、建议后续”三类结论，不夸大。

## 0. 修复状态（第一轮、第二轮部分 a 和 b、第三轮，都见本节）

第一轮修复时，下列文件同期另有修改，第一轮不碰，落在其中的发现标为“推迟”：`src/protocol/udp_connection.*`、`udp_demux.*`、`src/transport/ice.*`、`turn*`、`modules/file_transfer/`、`modules/clipboard/`、`modules/power/`、`components/supervisor/`、`src/session/share*.cpp`、`src/agent/video_pipeline.*`、`src/session/audio_stream.cpp`。“已修”指 Mac 上的构建和测试通过；Windows、Android、浏览器上的行为只编译过（Windows 交叉编译、Android 交叉编译、WebAssembly），还要真机或真浏览器验证，各条写明。

第二轮（b）修了第一轮推迟在 `udp_connection.cpp`、`ice.*`、`turn*` 里的 SA-01、SA-04 的其余部分、SA-09、SA-12，以及 3.8 节的参考项 F7（ICE 信令大小）和 F8（TURN）。线上格式变了（问候补齐、PIN 问候先要 cookie），网络协议的版本随之加一；信令格式没变。都只在 Mac 上构建和测试过（环回），Windows、Android、iOS 模拟器、WebAssembly 只交叉编译过。

| ID | 状态 | 怎么修的 / 为什么推迟 |
| --- | --- | --- |
| SA-01 | 已修（第二轮） | 观看端每个问候只验证一个答复：第一个走到 PIN 校验的答复用掉这次验证（`ClientHandshake::spent`），之后的答复（正确 PIN 的也一样）一概不验证；UDP 上它不对就立即以 `AUTHENTICATION` 失败，而 `AUTHENTICATION` 结束观看端的运行（不重连），所以冒充分享端的人（路径上的、TURN 或宿主通道的运营者、伪造分享端地址的）每次运行只能猜一次，和分享端一侧“每个回答一次”对称。代价写进了 `rcsdk.h`：伪造分享端地址发来一个 PIN 形式的答复也会让这次连接失败，和 PIN 错一样（路径上的人本来就能丢包）。TCP 本来就一条连接一个答复 |
| SA-02 | 已修 | 数组、对象最多嵌套 32 层，超出按格式错误拒绝并说明上限；数字只认 JSON 写法、必须有限；转成整型时越界取该类型的最小或最大值（不再是未定义行为）；`\u` 必须是 4 位十六进制、代理项必须成对。胶水层每个收 JSON 的入口都检查并给出明确错误 |
| SA-03 | 已修（第三轮） | 见本节“第三轮” |
| SA-04 | 已修（第一轮握手层，第二轮地址证明） | 第二轮：UDP 上分享端对 PIN 问候一律先回 Retry cookie，只回答带着有效 cookie（证明它能在自己的地址收到）的 PIN 问候，伪造源地址的问候拿不到回答、占不了次数；证明了地址却不完成的仍算一次失败。PIN 连接因此多一个往返。第一轮，握手层：PIN 尝试从问候起占用，只有分享端回答了、对方到结束都没证明 PIN 才算一次失败；回答之前就被拒绝的问候（CPace 份额为低阶点）不再计失败，此前任何人不猜也能用它锁死 PIN。回答了却不完成的仍算失败：回答本身就让对方能离线验证一次猜测，否则 `pin_attempts` 的保证失效。剩下的部分——只对已用 Retry cookie 证明地址的来源回答 PIN 问候，使伪造源地址的问候占不到次数——在 `udp_connection.cpp`，推迟（与 SA-09 的修法同一处：首个问候只回 Retry cookie，两条一并解决）；`tests/security.cpp::pinLockedOutByHellos` 仍演示这一部分，那边改完后改成断言持正确 PIN 的观看端能连上 |
| SA-05 | 已修 | 先打开并核对端点所在目录：属于本账户或 root，别的账户改不了其中的条目（组和其他人不可写，除非带粘滞位；Android 上应用自己的组可写），否则以原因失败；之后都相对这个目录的描述符操作。锁文件必须是本账户的、没有别的名字的普通文件（拒绝硬链接和符号链接）；已有的套接字不是本账户的就拒绝而不删；`bind` 后核对确是本账户的套接字，再用 `fchmodat(…, AT_SYMLINK_NOFOLLOW)` 设权限，不再按路径 `chmod`。macOS 示例的端点从组 `daemon` 可写的 `/var/run` 移到只有 root 能改的密钥目录 |
| SA-06 | 已修，有写明的残余 | 限定期间：按键和文字只在共享的应用在前、且（`WINDOW`）共享的窗口是它最前面的窗口时执行（Windows 上也可以是它拥有的对话框）；媒体键、Windows 键、Alt+Tab、Alt/Ctrl+Esc、Ctrl+Shift+Esc、Print Screen，macOS 的 Command-Tab、Command-空格、强制退出、锁屏、注销、截屏、Control+方向键一律拒绝（`INVALID_STATE`）；Windows 上指针按下不再接受被该应用别的窗口拥有的窗口。别的应用注册的全局快捷键拦不住，`rcsdk.h` 和指南 07 写明，并写明限定不是安全边界。键表在 Mac 上单元测试；注入路径只编译过，要在 Windows 和 Mac 真机上试 |
| SA-07 | 已修 | 新增 `rcs_agent_info.program`：macOS 取自连接的审计令牌（`LOCAL_PEERTOKEN` + `proc_pidpath_audittoken`，令牌带 exec 时更新的进程版本），会话在助手说完握手后再核对一次令牌，期间把连接交给别的进程或 exec 成别的程序的连接在问宿主之前就以 `AUTHENTICATION` 拒绝；Windows 取自连接时打开的进程；Linux/Android/iOS 为空。实测（本机）：令牌指“最后使用这条连接的进程、现在的样子”，因此必须在对方说话之后核对，这一点写进了头文件。macOS 示例改为按 `program` 判断，链接密钥只有 root 能读，控制台用户另有一份只属于自己的 0600 副本 |
| SA-08 | 第二轮已修 | 见本节“第二轮” |
| SA-09 | 已修（第二轮） | 问候数据报补零到和 `ServerHello` 一样大（`0xD1 问候 cookie(16) 零`，没有 cookie 时那 16 字节是零），大小不对的问候不回答，所以对没证明地址的来源，回答（`ServerHello` 1.00×、Retry、版本通知、PIN 通知）都不比它发来的大。迁移对未证明地址“最多三倍”的限额（`sendProbe`）原样保留：`PATH_CHALLENGE` / `PATH_RESPONSE` 只回应认证过的数据报 |
| SA-10 | 已修（报错清楚；抢占本身仍是部署问题） | 名字被占时原因说明是谁：Unix 上是占着套接字或锁文件的账户（不去连对方，免得给真正的监听者留下一个要丢弃的连接），Windows 上是占着管道名的进程和账户（对方的访问设置允许查询时）。`agent.h` 与指南 16 写明怎样让别的账户占不到名字（只有会话账户能改的目录；Windows 每次运行随机名并经看护者交给助手——看护者的改动属 `components/supervisor/`，推迟） |
| SA-11 | 第二轮已修 | 见本节“第二轮” |
| SA-12 | 已修（第二轮） | `ice::Agent` 剔除（写日志，失败原因里列出）对端信令中没有对端会在的候选：未指定、组播、广播和保留地址、云元数据服务（169.254.169.254、100.100.100.200、168.63.129.16、fd00:ec2::254，IPv4 映射和 NAT64 地址里带的也算）；环回和链路本地的只在本端自己也有同一范围的候选时才用；同一个 IP 最多 4 个。来自剔除地址的检查既不回答也不学成对端反射候选。新的 `rcs_network_tuning_config.ice_addresses`（`RCS_ICE_ADDRESSES_LOOPBACK`）让本端也给出 127.0.0.1 和 ::1，供同一台机器上的测试。没做：信令速率限制（同时 32 次、排队 256 份的上限照旧）；子网定向广播认不出（不知道网段掩码；套接字没开 `SO_BROADCAST`，系统本来就不发） |
| SA-13 | 已修 | Android 上只信任系统自带、用户没停用的根证书，不再读 `cacerts-added`；需要私有 CA 或用户安装证书的中继用现有的 `turn_certificate_sha256` 指纹。读目录的函数可在任何 BearSSL 构建上测；要在 Android 真机上确认 TURN over TLS 照常连上系统 CA 签的中继 |
| SA-14 | 已修 | 契约改为宿主只保留最近给它的 `RCS_MAX_CURSOR_SHAPES`（32）张指针图像，先给的先丢；会话在一张图像离开宿主保留的范围后再出现时重新附带它。浏览器包和 QML 示例按同样顺序淘汰。核心逻辑单元测试；浏览器侧要在浏览器里实测 |
| SA-15 | 已修 | 密钥、PIN、口令不再写进选项 JSON，单独以字节交给模块，调用后在模块（共享）内存里清零再释放，`createSession` 编码出的副本也清零；胶水层的 `rcs_identity` 副本无论怎样返回都清零。指南 23 加了“不放第三方脚本 + 严格 CSP”一节和起步用的 CSP（按包的代码推出，未在各浏览器实测）。核心在会话活着时保留的副本不清零（重连要用），已写明 |
| SA-16 | 第二轮已修 | 见本节“第二轮” |
| F11（JSON 转义） | 已修 | 见 SA-02 |
| F14（浏览器剪贴板同步） | 已写进文档 | 指南 23 写明 `receive` + `ClipboardSync` 与 `readOnFocus` 意味着信任对方 |
| F7（3.8 节，ICE 信令大小） | 已修（第二轮） | 比能有的最长信令（1419 个字符：最长凭据、32 个 IPv6 候选的重启信令）还长的文本不解码，直接 `INVALID_ARGUMENT` |
| F8（3.8 节，TURN 长期凭据与 LIFETIME） | 已修（第二轮），有残余 | 按服务器给的有效期过一半时刷新，至少隔一秒（给 0、1 秒的不再让它每拍都刷新；没按“至少 60 秒”钳：符合 RFC 的服务器不会低于 600 秒，测试用 2 秒）；438 只换新 nonce 重发一次，重发的又是 438 就以 `NETWORK` 结束，不再打环；配了用户名时没要凭据就批下的分配不接受，到 `turn_allocate_ms` 以 `AUTHENTICATION` 说明。残余：认证前的 4xx 仍可结束中继（长期凭据机制如此，看得见事务 ID 的在途者本来也能丢包） |
| F4、F9（3.5、3.8 节）、F12、3.6 节的 F7～F9、证书细节 | 第三轮已修或写明 | 见本节“第三轮” |
| F13、3.6 节的 F6 | 参考，不改 | F13：信令的保密归宿主的通道（第 2 节，控制面）；F6：同账户的进程本来就能停掉自己的助手 |

**每条修复的测试**（修复前失败、修复后通过）：SA-02 `tests/web_json.cpp`（嵌套上限、数字写法、`\u` 与代理项、整型范围；旧解析器在一百万个 `[` 上栈溢出）；SA-04 `tests/secure_channel.cpp` 的 `pins`（五个低阶份额的问候不计失败、正确的 PIN 照常进入，回答后不完成的仍计一次）；SA-05、SA-10 `tests/agent.cpp` 的 `endpointPlaces`（他人或组可写的目录被拒、粘滞目录可用、端点处的符号链接和锁文件的硬链接或符号链接被拒且目标不变、名字被占时原因说明是哪个账户）；SA-07 `tests/agent.cpp` 的 `peerPrograms`（macOS：用 fork 的子进程，被接受后把连接交给另一个进程、或 exec 成 `/bin/sleep` 的，`confirm` 以 `AUTHENTICATION` 拒绝；接受前就 exec 的报告的是新程序）；SA-06 `tests/keys.cpp` 的 `confinement`；SA-13 `tests/tls.cpp` 的 `androidStore`（只在 BearSSL 构建上编译，本轮在 Mac 上另配一个 BearSSL 构建跑过）；SA-14 `tests/cursor_stream.cpp`（宿主按 32 张先进先出保留时，任意顺序的两千次指针变化里它始终有要画的那张）。SA-15 没有自动测试：浏览器包没有 JS 测试设施，只确认了 WebAssembly 构建和 `node --check`。`tests/security.cpp` 第一轮没有改：它的 SA-01、SA-09 两项和 SA-04 的地址部分等第二轮。

**第二轮的测试**（各自在 Mac 上跑过；“修复前失败”是把相应修复去掉后重新构建、跑同一个测试确认的）：SA-01、SA-04、SA-09 在 `tests/security.cpp` 里从演示改成了回归测试——`viewerTestsOnePinGuess`（冒充者发 400 个答复、正确 PIN 是第 351 个：观看端验证第一个后以 “Wrong PIN or another secret” 失败，用时约 20 ms，没有发出 finished；去掉两处检查后观看端照旧用第 351 个完成握手，测试失败）、`pinHellosProveAddress`（6 个不取 cookie 的 PIN 问候各得一个 Retry、没有回答；取了 cookie 不完成的算一次失败；持正确 PIN 的观看端连上；去掉 PIN 的 cookie 条件后陌生人直接得到回答，测试失败）、`helloAnswersReflectNothing`（100 份 325 字节的问候各得 325 字节的回答，1.00×；旧的 139 字节问候得不到回答；放宽大小检查后测试失败）；`tests/secure_channel.cpp` 的 `pins` 在握手层验证同一规则（低阶份额不用掉验证）。SA-12、F7 在 `tests/ice.cpp`：`peerAddresses`（6 个环回监听端加 14 个各类地址：不开选项时一个检查也到不了监听端，失败原因逐个列出剔除的地址和原因；开 `RCS_ICE_ADDRESSES_LOOPBACK` 时正好 4 个监听端收到检查；只对环回部分做了“去掉修复”的确认，其余地址在旧代码下会真的发出探测，所以不在本机跑）、`signals`（最长的信令能解，长一个字符或 50 MB 的在 500 ms 内以 “longer than any signal” 拒绝；旧代码把长一个字符的当作有效信令）；`tests/c_api.c` 检查 `ice_addresses` 的取值。F8 在 `tests/turn.cpp`：有效期 0 时 1.5 秒内只刷新一次（旧代码每拍一次）、对 CreatePermission 一直回 438 时以 NETWORK 结束（旧代码一直重发）、`unaskedCredentials`（不认证的服务器、带用户名的客户端：`AUTHENTICATION` 并说明；旧代码接受分配）。另跑了 `udp`、`session.pin`、`session.udp`、`host_channel`、P2P / 中继 / 迁移 / 备用路径的会话测试，最后全套测试（`ctest --preset release -E capture_targets -j4`，157 项）通过；Windows（llvm-mingw）、Android、iOS 模拟器、WebAssembly 交叉编译通过。

**要在真机上看的**：Mac 与 Windows（同一局域网）、iPad、S25+ 之间用 PIN 的 UDP 和 P2P 连接（多一个往返，应照常连上；输错 PIN 应立即失败）；P2P 在真实网络（家用 NAT、IPv6、Tailscale 这类 100.64/10 地址）上照常找到路径、日志里没有误剔的候选；有凭据的 coturn 中继照常分配和刷新。

### 第二轮（部分 a）

本轮不碰 `src/protocol/udp_connection.*`、`udp_demux.*`、`src/transport/ice.*`、`turn*`、`src/session/connect.cpp`（同期另有修改）。文件传输模块的版本随之加一（两半之间的消息变了，和对端之间的格式不变；两端仍须同一版本）；会话协议、剪贴板模块版本不变。

| ID | 状态 | 怎么修的 / 为什么推迟 |
| --- | --- | --- |
| SA-11 | 已修 | 一端交给自己存储（机器那一半或宿主回调）的读写，在存储回答之前一直算数。接收方逐块记着交给存储、还没回答的写（存储按顺序回答），重发的 `OFFER` 不再清零：纪元立即加一，等存储写完手里的再回 `ANSWER`，这之前到的块和条目结束都丢掉；超过窗口加一块的块也丢掉并照样等存储写完再回 `ANSWER`（不再算违反规则：正常发送方在某项失败后可能已发下一项）。`conclude` 不再在条目结束时减掉还在写的字节（发送方先发 `ITEM_END` 报错再发下一项，原来能每项多排一个窗口）。发送方同样（新发现，见下）：`ANSWER` 不再清掉交给存储还没读回来的块，标成不发、等它们回来，窗口照算；`READ` 方式的取收到 `ANSWER` 算违反规则。测试 `file_transfer.flow`（新）把会话那一半单独跑起来、测试自己当对端和慢盘：8 次重发后接收方存储排着的写从 34 MiB 降到 4.25 MiB，发送方的读从 128 个降到 16 个；存储写完、读回后照常继续。会话场景 `file_transfer_reconnect`、`file_transfer_agent` 等照常通过 |
| SA-16 | 已修 | 机器那一半的回答（条目结果、传输结束原因，会原样发给对端）和模块的日志只用条目在线上的相对路径：收的一端是接收目录下的路径，目录本身叫“the receiving directory”；发的一端是条目在线上的名字（宿主给条目改了名时本机文件名也不露出），列目录时出错的条目叫“Entry 序号”。`files.hpp` 的 `Directory` 只记消息里怎么称呼，读请求带上条目在线上的名字；传输没做完就离开的日志不再写目录和记录的路径。测试：`file_transfer.store` 的 `pathsStayHere`（发、收、丢弃十种失败的原因里没有测试目录的路径、没有本机文件名）、`file_transfer.flow` 的 `logsNameNoPath`、会话场景 `file_transfer`（对端收到的“空间不够”原因里没有接收端目录）和 `file_transfer_agent`（日志里没有目录）。以前这些原因里是 `/Users/<用户名>/…` 这样的绝对路径 |
| SA-08 | 已修（Windows 上以 SYSTEM 跑过测试；真的 Ctrl+Alt+Del 待验证） | 每个被看护进程一条自己的中转管道：看护者建好（随机名、`FIRST_PIPE_INSTANCE`、一个实例、DACL 只有 SYSTEM 和管理员），自己打开并连上客户端那一头，作为继承句柄交给被看护进程（`RCSDK_SUPERVISOR_SAS` 里是句柄，不再是名字），启动后关掉自己的副本。别的进程（同账户的也一样）打不开名字、占不住实例，不能替它请求、也不能让它的请求发不出去；发到哪个会话按看护者持有的被看护进程句柄的令牌，不再按管道报告的进程号（可能被复用）。被看护进程退出时中转结束并放开管道，之后的请求立即失败；不是 `'S'` 的请求回 `INVALID_ARGUMENT`、什么都不发。被看护进程一侧（`rcs_helper_*`、Ctrl+Alt+Del）用继承的句柄 `TransactNamedPipe`。测试：`rcsdk.supervisor.run`（只在 Windows 上以 SYSTEM 运行，否则跳过）里每个被看护进程检查自己那一头有回答、按名字打不开任何 `rcsdk-sas-*` 管道（修复前用户方式的被看护进程能按名字打开，变量里也不是句柄）。在一台 Windows 11 PC 上以 SYSTEM 计划任务运行通过；测试只看本看护者的管道（进程号在名字里），那台 PC 上另有一个旧版本的看护者，它的管道 SYSTEM 能打开 |
| 原生文件传输（残余风险第 2 条） | 已逐行审阅，错的已修 | 见下面“第二轮的审阅结论” |
| 剪贴板的隐藏 / 瞬态数据（残余风险第 3 条的一部分） | 已逐行审阅，正确；加了测试 | 见下面“第二轮的审阅结论”；图像 / HTML / RTF 解码与方向规则本轮没有再审 |

**第二轮的新发现**（都在第二轮修了，除了 SA-17）：

- **SA-11 的发送端一半〔中〕**：已认证的接收方反复发 `ANSWER`，发送方每次清掉交给存储的读、从头再读一个窗口，慢盘前排着的读请求（以及读出来再丢掉的 256 KiB 数据）随 `ANSWER` 的次数无限增长；和 SA-11 同一个修法。
- **条目失败绕过窗口〔中，SA-11 的一部分〕**：接收方在条目结束（发送方的 `ITEM_END` 报读失败，或本端写失败）时把那一项还在写的字节从“没写好”里减掉，发送方随即发下一项，反复如此每项多排一个窗口；现在只在存储回答时减。
- **发送端列目录的 TOCTOU〔低〕**：列要发的目录时按路径（`opendir`、`FindFirstFileExW`、`lstat`），能在共享树里写的进程在看过和列出之间把子目录换成链接，链接后面任何本账户能读的目录的文件名、大小、时间就进了发给对端的清单（内容已经经句柄读，读不到）。现在经句柄列（POSIX `openat(".")` + `fdopendir`，Windows `GetFileInformationByHandleEx(FileFullDirectoryInfo)`），被换掉的以 `RCS_FILE_OTHER` 列出。测试 `file_transfer.store` 的 `swappedWhileListed`（经测试接缝在看过之后换掉）。
- **不说续传的清单动了别人留下的续传文件〔低〕**：机器那一半准备时不看清单是不是续传；发送方（令牌是它选的）用别人留下的令牌发一个“新”清单，宿主看到的不是续传，机器那一半却接着别人写了一半的文件写，或者当成以前的运行留下的删掉。现在准备的请求带“续传”标志，不是续传而目录里有这个令牌的记录或临时文件就整次失败（`INVALID_STATE`），什么都不拿、不删；头文件写明令牌只是名字、不是凭据，宿主只接受当初那个对端的续传清单。测试 `file_transfer.store` 的 `resuming`。
- **同时在收的传输不一起算剩余空间〔低〕**：每次只和剩余空间比，几次同时接受的传输各自都过得去。现在机器那一半按卷记着每次传输还要写多少，一起比，原因写明。测试 `file_transfer.store` 的 `sharedSpace`。
- **SA-17〔低，Windows，第三轮已修（只交叉编译）〕8.3 短名别名**：Windows 上按名字打开不分大小写、也认 8.3 短名，`on_conflict = OVERWRITE` 时一个形如 `REPORT~1.TXT` 的条目名会替换接收目录里短名相同、长名不同的文件（宿主看到的条目名和被替换的文件名不同）。修法是打开后用 `FILE_NAME_INFO` 核对实际的名字，或在路径规则里拒绝短名形式的名字（会拒掉少数合法名字，要决定）；只能在 Windows 上测，本轮未做。

**第二轮的审阅结论**（逐行读 `modules/file_transfer/src/files_posix.cpp`、`files_windows.cpp`、`store.cpp`、`record.cpp`、`session_half.cpp`，`modules/clipboard/src/session_half.cpp`、`content.cpp`、`system_*.{cpp,mm}`、`machine_half.cpp`）：

- 符号链接 / 目录联接的竞争：收的一端 POSIX 全部经 `*at()` 和 `O_NOFOLLOW`；Windows 每一段用 `NtCreateFile` 带 `RootDirectory` 和 `FILE_OPEN_REPARSE_POINT` 相对打开、打开后查名字代理的重分析点，目录句柄不共享删除、并持有上一级，建（`FILE_CREATE`）、写（链接、非磁盘文件、硬链接数都查）、改名（`FileRenameInformation` 带 `RootDirectory`，替换时替换的是链接本身）、删、设时间都对句柄做：正确。发的一端读文件经句柄，列目录原来按路径（已修，见上）。名字规则拒绝 `:`（备用数据流）、`\`、保留设备名、结尾的点和空格。8.3 短名见 SA-17。
- 覆盖规则：`OVERWRITE` 不替换目录，`RENAME` 和不替换的改名在 macOS 用 `renameatx_np(RENAME_EXCL)`、Linux/Android 用 `renameat2(RENAME_NOREPLACE)`、Windows 用 `ReplaceIfExists = FALSE`，原子、不会覆盖别人刚放的文件；`SKIP`、`FAIL` 在准备时和改名时都查：正确。
- 续传令牌：16 字节，`rcs_generate_connection_key`（系统的密码学随机数），猜不到；但由发送端选、和对端身份没有绑定，模块也无从绑定（运行编号每次运行都变）。已修：只有说了续传的清单才接着写，宿主的判断写进头文件。
- 磁盘耗尽：对端写不出清单声明的大小（越界的块违反规则）；准备时查剩余空间（已改成同时在收的一起算）；留下的续传文件只在宿主接受时占空间、`rcs_file_discard` 删。磁盘是否留余量是宿主的事，写进了头文件和指南。
- 新建文件和目录的权限：`0666` / `0777` 去掉 umask（POSIX），Windows 继承目录的访问控制，和任何程序存文件一样；临时文件和记录也一样。宿主收到其他账户能写的目录里时，那些账户能改其中的东西（带粘滞位时也能在传输要用的名字上先放自己的文件，续传会接着写）；没有加属主检查，因为有的卷上看到的属主不是写它的账户（FAT、一些版本的 Android 共享存储），查了就续不上，又没法在那些卷上验证。写进头文件：收到只有本账户能写的目录。
- 剪贴板的隐藏 / 瞬态复制：读到的复制带隐藏标记（Windows 的 `ExcludeClipboardContentFromMonitorProcessing` 或历史 / 云剪贴板为 0，macOS / iOS 的 `ConcealedType`、`TransientType`，Android 13 起的 `EXTRA_IS_SENSITIVE`）时，只有 `send_concealed` 为 1 才发，按每个对端查，`send_existing` 给新对端的那份、分享端把观看端的复制转给其他观看端的那份也查；放到本机剪贴板时带同样的标记（Android 12 及以前没有这个标记，系统的剪贴板历史可能留下，平台的限制）。日志和事件的原因里只有格式、大小，没有内容；复制的文件只有名字上线，本机路径只在两半之间。正确；会话场景 `clipboard` 加了“日志里没有隐藏复制的文字”的检查。

### 第三轮

第三轮修前两轮推迟的其余各项和残余风险第 3、4、5 条。这一轮没有改会话的线上格式和信令格式（不升协议版本），模块版本也没变（剪贴板的线上形式没变，只是收的一端对文件名查得和文件传输一样严，同版本的发送端本来就发不出那样的名字）。都在 Mac 上构建和测试（环回）。Windows 的改动（SA-17、看护者的映射盘、3.6 F9、Windows 的证书检查）只用 llvm-mingw 交叉编译过；浏览器的改动（F4、F12 的页面）编出了 WebAssembly 包、在 node 里跑了能跑的，都要真机或真浏览器验证（各条写明）。

| ID | 状态 | 怎么修的 / 为什么不改 |
| --- | --- | --- |
| SA-03 | 已修 | 等 `receive()` 取走的消息最多 `UdpOptions::maxInbound`（32 MiB；队列空时一条总放得下）。超过时会话落后了（宿主的回调占着线程，或已认证的对端灌消息）：可靠流像 TCP 一样等（按序的下一条留在原处，提前到的最多 4096 条、并新加 16 MiB 的上限，再后来的不确认、由对端重发，于是对端的窗口让它停下）；完整的画面按丢失处理（这一路下一帧之前给会话一个 `StreamGap`，计入 `frames_lost`）；声音和不可靠数据按丢失的数据报处理。日志说明会话什么时候落后、什么时候赶上、其间丢了多少。宿主看到的和网络丢包一样（`RCS_EVENT_RECOVERY`、不可靠数据本来就可能丢），可靠消息一条不丢 |
| SA-17 | 已修（只交叉编译） | Windows 上传输要替换的名字（改名的目标）和条目路径上的每个目录，必须正是那里东西的长名（大小写之外）：经目录自己的一个新句柄用 `NtQueryDirectoryFile`（`FileBothDirectoryInformation`，名字作模式）查。是别的东西的 8.3 短名时这一项以 `INVALID_STATE` 失败，原因写明，那个文件不动。`RENAME`、`SKIP` 本来就不动它（不替换的改名会冲突）；长名本身是 8.3 形式的照常替换。没有选“在路径规则里拒绝短名形式的名字”：那会拒掉合法的名字，而别的卷、别的系统根本没有短名 |
| F4（3.5） | 已修（没在浏览器里跑） | 每个包作为解码器编号的块交给 WebCodecs（块的时间戳就是编号，画面带着它）；只留 `decode()` 正在等的那个块的画面，别的（同一块的第二帧，如显示两帧的 AV1 时间单元；等待已放弃的块迟到的画面）一到就关掉，不再堆积。画面尺寸不是包说的（流的，到达时已查过）就拒绝：`RCS_ERROR_CODEC`，两个尺寸都写明，那帧关掉；观看端换一次解码器，再错就结束这次运行。worker 按检查过的尺寸设画布、画画面。逻辑在 `src/platform/web/decoded.hpp`，离开浏览器也能测 |
| F12 | 已修 | `serve.mjs` 默认只听环回（`--listen` 才听别处）；中继是它自己的设置（`--relay`），页面不能再用 `relay=` 让它连任意地址（原来是开放的 TCP 中继，任何网站的页面都能借它连内网）；进房间要令牌（`--token`，或启动时生成并打印）：页面把令牌放在请求体里 POST 到 `/ticket`，换一张 10 秒内只开一次 WebSocket 的票；票和 WebSocket 只给它自己的页面（`Origin`）；一条消息最多 16 MiB；页面带 `Referrer-Policy: no-referrer`。`viewer.html` 不再从 URL 读密钥（也不读令牌），会话拿到密钥后清空密钥框；测试从控制台 `viewer.connect({room, token, key})`。指南 23 写明照抄它的宿主要留着什么 |
| 3.6 F7 | 写明，不加开关 | `on_input` 先看到每个事件、可以丢掉它，`host_input_types` 能把这一类交给宿主逐个决定，这已经是开关；再加一个字段只是同一件事的第二种写法。`rcsdk.h`（`SECURE_ATTENTION`）和指南 16 写明有控制权的观看端都能发、宿主怎样不让它发。默认行为不变 |
| 3.6 F8 | 已修 | 看护者的程序必须在本机的盘上：`\\`、`//` 开头的路径（共享、`\\?\`、`\\.\`）在所有系统上都是 `INVALID_ARGUMENT`（原因写明），Windows 上映射到共享的盘符（`DRIVE_REMOTE`）在创建时拒绝。`supervisor.h` 和指南 16 写明 |
| 3.6 F9 | 已修（只交叉编译） | `secure_desktop` 关时，每个输入事件执行前先看接收输入的是不是用户自己的桌面（`OpenInputDesktop` 和它的名字，几次系统调用），不是就不执行（`PERMISSION_REQUIRED`，和跟随失败时一样）；每秒一次的检查照旧暂停控制权。正常路径只多这几次调用；检查和 `SendInput` 之间剩下的几微秒仍由 Windows 兜底（线程不在输入桌面上时它拒绝注入） |
| F9（3.8，证书细节） | 已修一部分，其余写明 | 真实的缺口：BearSSL 的 minimal 引擎不读扩展密钥用法，且没有主题备用名称时退回到主题的 CN；Windows 的 SSL 策略也退回到 CN；Apple 两样都不接受。现在三个后端都拒绝不是 TLS 服务器的证书：写了用途却不含服务器认证的，和只在主题里写服务器名、没有 subjectAltName 的。BearSSL 在链通过后读服务器证书（`serverAuth` 或 `anyExtendedKeyUsage`；查名字时要有 subjectAltName），Windows 在策略检查前查 subjectAltName，三边原因用同一句话（`TrustProblem::use`）；pin 仍然只认那张证书。不改：吊销检查（三个后端都不查：只放行 TURN 的网络常常挡掉 OCSP / CRL，查不到时只能放行或连不上；要紧的宿主 pin 证书），宿主自己的 CA 证书包（私有 CA 的宿主 pin 叶子证书，续期时重新 pin） |
| 剪贴板（残余第 3 条） | 已逐行审阅，错的已修 | 见下面“第三轮的审阅结论” |
| 信令限速（残余第 4 条） | 不做，写明 | 按发送者限速要知道是谁发的：SDK 只看到宿主给的路由号，宿主才知道（控制面）。SDK 已经限制每个信令的大小（1419 个字符）、同时处理 32 个、排队 256 个（等得最久的让位）。`rcs_session_signal` 的注释写明每个信令要一次 ICE 交换，替陌生人中转信令的宿主要限制一个发送者多久能发一次 |
| 有状态模糊（残余第 5 条） | 已加，跑过，没有发现 | 见下面“第三轮的模糊测试” |

**第三轮的测试**（各自在 Mac 上跑过；“修复前失败”是把修复去掉后重新构建、跑同一个测试确认的）：SA-03 `tests/udp.cpp` 的 `behindTheSession`，三部分各自在去掉上限后失败：12 MiB 可靠数据发往一个不读的服务器（上限设 1 MiB），发送方得到确认的不超过上限、4096 条提前到的和它自己的窗口，之后读时全部按序到达；已认证的对端灌 8 MB 不可靠数据、再发一条控制消息，排在控制消息前面的不超过上限；30 个 100 KB 的画面，超出上限的变成下一帧之前的一个 gap，`frames_lost` 记上。SA-17 `file_transfer.store` 的 `shortNames`（只在 Windows 上编译，卷不生成短名时跳过）：`OVERWRITE` 用另一个文件的短名失败、那个文件内容不变，`RENAME` 存到旁边，路径上的目录用短名时失败，长名本身是 8.3 形式的照常替换。F4 `web_decoded`（尺寸；没人等的、同一块第二帧的、迟到的画面不留，一个块的一千帧只留一帧；错误照留）。F12 `web_serve`（node，端口由系统选：默认只听环回；错的令牌、别的网站、没有 `Origin`、令牌放在 URL 里都拿不到票；没有票或别的网站的页面开不了 WebSocket；一张票只开一次；页面的 `relay=` 不被跟随；字节两个方向原样走；声称超过 16 MiB 的消息关掉连接；对旧的 `serve.mjs` 九项失败）。3.6 F8 `supervisor.api`（四个共享 / 设备路径被拒，修复前被接受）。证书 `tls` 的 `serverUse`（第二个测试 CA 和它签的两张 `turn.rcsdk.test` 证书：只有客户端认证的，没有 subjectAltName 的；Apple 后端和本机另配的 BearSSL 构建都拒绝并说明，去掉检查后 BearSSL 两张都接受）。剪贴板 `clipboard.content`（文件传输不收的名字被拒、正常的名字照收；预取的规则，含会绕回的和）、`clipboard.pasteboard_macos`（测试自己的粘贴板：64×32 的 PNG 旁边有 TIFF，9000×9000 的没有；修复前有）。3.6 F7、F9 没有自动测试（F9 要 Windows 锁屏）。另跑了 `udp`、`security`、会话场景 `udp`、`data`、`host_datagram`、`viewers_udp`、`parity`、`lost_pictures`、五个剪贴板场景；最后全套测试（`ctest --preset release -E capture_targets -j4`，160 项）全部通过。Windows（llvm-mingw）、Android、iOS 模拟器、WebAssembly 交叉编译通过，`ctest --preset wasm`（node）146 项通过（107 项是要套接字或编码器的会话场景，照旧跳过）。`security` 在本机负载很高时有一次 PIN 用例超时失败，单独重跑三次都通过（握手的代码这一轮没动）。

**要在真机和浏览器里看的**：Windows 上 `rcsdk_test_file_transfer_store.exe`（`shortNames` 跑到而不是跳过：`fsutil 8dot3name query C:` 要显示生成短名）、`rcsdk_test_tls.exe`（Windows 后端照常接受测试证书；`tls_relay` 对 Mac 的 TLS 终结器照常连上）、锁屏时的输入（`secure_desktop` 关、观看端一直在打字时按 Win+L：锁屏上不出现任何字符，`rcs_input_stats.last_result` 是 `PERMISSION_REQUIRED`，解锁后控制权自动回来）、看护者（`supervisor.run` 以 SYSTEM 照常通过）；浏览器（Chrome）里 `viewer.html` 经新的 `serve.mjs`（填令牌）照常看、控、传文件，1080p60 硬件 H.264 每帧照常出画面、画布是流的尺寸。

**第三轮的审阅结论**（剪贴板，逐行读 `modules/clipboard/src/content.cpp`、`messages.cpp`、`session_half.cpp`、`machine_half.cpp`、`pastes.cpp`、`system_macos.mm`、`system_windows.cpp` 的图像部分、`stand_in_windows.cpp` 的文件描述）：

- 线上解码：长度、格式顺序和唯一、TEXT / HTML 是严格 UTF-8、PNG 头（最大 16384×16384）、文件 1..100 000 个且名字唯一、没有尾随字节，都对。文件名原来只拒绝控制字符、`/`、`\`、`.`、`..`，比头文件说的“文件传输的路径规则”松：`x:y`（Windows 上是 `x` 的数据流）、`CON`（设备）、`x.`（Windows 去掉结尾的点，成了 `x`）、带 `* ? " < > |` 的、文件传输自己的 `.rcsdk-part` / `.rcsdk-resume-` 名字都能进来，而预取到的文件正是按这些名字放到剪贴板上的。已修：按文件传输的规则查。
- 大小：`max_bytes` 两端都管，放不下的格式按 TEXT、HTML、RTF、IMAGE、FILES 的顺序留下；预取的“总共多少字节”原来是把对端说的大小直接相加，能绕回。已修（见下一条）。
- 图像：Windows 放在 PNG 旁边的位图解码前限 64 Mpx（256 MiB）；macOS 放在旁边的 TIFF 原来在本进程里解码对端的 PNG 而没有限制（16384×16384 的小 PNG 要解出 1～2 GiB，再存一份同样大的 TIFF）。已修：和 Windows 一样，超过 64 Mpx 的只放 PNG（粘贴板照样能把 PNG 转给要 TIFF 的程序）。iOS 只放 PNG、不解码；Android 不传图像。
- 方向：没开 `RECEIVE` 时到达的复制丢掉；从对端来的复制不会再发回它，分享端把一个观看端的复制转给其他观看端时跳过来源；`on_clipboard` 先看每份复制、可以不让它放上或发出：正确。
- 没有用户动作的写入：`RECEIVE` + `SYSTEM` 时对端的每份复制都放到本机剪贴板（`on_clipboard` 可以否决），这是设计（指南写明，F14）；对端可以连着发复制、反复改本机剪贴板，宿主要限制就在 `on_clipboard` 里限。预取（`files_prefetch_bytes`）在没人粘贴时就取文件：原来按复制里说的“只有文件、共多少字节”决定，取的却是对端公布时列出的东西（可以更多、更大，或有目录）。已修：列出后按列出的查，只有文件且总共不超过 `files_prefetch_bytes`，否则取消这次取（`RCS_CLIPBOARD_PASTE`，`BUFFER_TOO_SMALL`，原因写明），留给粘贴时再取。

**第三轮的模糊测试**：`tests/fuzz/`（`RCSDK_BUILD_FUZZERS=ON`；Homebrew LLVM 23 的 libFuzzer，ASan + UBSan，`-fno-sanitize-recover=undefined`；怎样构建和运行见 [BUILDING](BUILDING.md) 的“模糊测试”），三个有状态目标，各按输入写出的一串步骤驱动 SDK 的一块：`udp_video`（观看端的 UDP 连接经 `UdpFuzzEnd` 收按分享端密钥封好的数据报：按分享端的算法算好冗余的合法画面、有的分片丢掉或发两遍，输入自己拼的分片、冗余和 GONE，可靠消息，任意帧，时间流逝，会话取走或不取）、`secure_channel`（加密记录流：下一条记录、序号相近的、输入给的长度和边界值、改了一个字节的、截断的、原始字节，每条载荷再按消息解码）、`file_transfer`（文件传输会话那一半的状态机：新的、重发的、续传的报价，回答和纪元，块，条目的结束和结果，取消和暂停；存储对或错、按序或乱序的回答；宿主的接受、拒绝、取消、暂停、发送；对端和机器那一半的离开和回来）。在一台 Mac 上各跑 5 分钟：`udp_video` 614 469 次（1992 条边，`fec::recover`、`receiveParity`、`recover` 都跑到），`secure_channel` 281 453 次（602 条边），`file_transfer` 664 698 次（3357 条边）；把 `udp_video` 的 `maxInbound` 降到 64 KiB 让“会话落后”也跑到后又跑 4 分钟 376 324 次（2065 条边，“落后”的路径也跑到）。都没有发现。没做：纳入 CI；第一轮的无状态 `fuzz_parsers` 没有入库。

## 1. 范围与方法

- **版本**：评估针对当时的开发版本，之后按第 0 节分三轮修复；正文的 `文件:行` 指评估时的代码。评估只新增测试与本文件，不修改生产代码。
- **组件**：握手与密钥协商、认证加密记录通道、UDP/TCP 传输与分片/FEC/重传、协议消息解码、编解码码流边界检查、模块机制与官方模块、文件传输、剪贴板、指针流、数据通道、助手链接与看护者（Windows 服务 / macOS LaunchDaemon）、远程输入与控制授权、无人值守、日志、浏览器（WebAssembly）包、第三方库（Monocypher、BearSSL、libvpx/dav1d/libaom/OpenH264、libopus）。
- **平台**：macOS、Windows、iOS、Android、浏览器/WebAssembly。Linux 上只有转发器在支持范围（它是服务器；分享和观看两端不支持 Linux），它作为 Linux 服务怎样运行见 3.9。Windows/iOS/Android 的平台代码只做了阅读审查，未在真机上验证（评估环境只有一台 Mac：不使用 Windows PC、安卓设备/模拟器、iOS 模拟器、虚拟机；不抓屏、不读写系统剪贴板、不出环回网络、不改安全设置）。
- **方法**：
  1. 对每个组件做人工代码审阅，记录 `文件:行`。
  2. 威胁建模（见第 2 节）。
  3. **定向测试**：新增 `tests/security.cpp`（仅环回），复现并量化握手类发现（SA-01/SA-04/SA-09）。
  4. **模糊测试**：用 Homebrew LLVM 以 ASan+UBSan+libFuzzer 重新编译 SDK（不入库），对所有“从网络、助手链接、STUN/TURN、信令、浏览器页面进入”的解析器写了一个统一 harness（`fuzz_parsers`），跑约 **1.06×10^7** 次执行、覆盖约 4264 条边、语料 1888 条。
- **覆盖说明（诚实声明）**：TURN over TLS 的证书/主机名校验（三后端）、STUN/TURN/ICE 解析、浏览器包与日志已由专项审阅配 ASan/UBSan harness 覆盖（结论见第 3.5、3.8 节与“已检查且稳健”）。**仍覆盖不足、需后续**：原生（非浏览器）文件传输的符号链接/junction 的 TOCTOU、覆盖规则、断点续传令牌等**语义**安全，以及剪贴板的隐藏/瞬态数据标记与图像/HTML/RTF 解码——其解析入口已模糊测试跑干净、浏览器侧文件名组件校验已确认，但上述业务规则本轮未逐行确认，列入第 5 节残余风险。模块的专项审阅确认了 SA-11，其余细目在本轮未能全部汇总，续审从该处接续。

## 2. 威胁模型

**资产**：屏幕内容、输入控制权、传输中的文件、剪贴板内容、密钥/PIN/口令、以及在无人值守下被远程操作的机器本身。

**信任边界**：
- 控制面（身份、配对、审批、信令中转、UI）归宿主；数据面（采集、编解码、加密传输、码率、输入、声音）归 SDK。
- 本机进程边界：会话进程 ↔ 助手进程（agent link）、看护者（Windows SYSTEM 服务）↔ 助手、CAD 中转管道。
- 共享秘密经宿主业务通道交换：能读该通道者可冒充任一端（文档已声明，与宿主信任自己服务器一致）。

**攻击者**：
1. 在途网络攻击者（运营商、同热点、中继/TURN 运营者、宿主信令通道）。
2. 未认证的远端。
3. 已认证但恶意的对端（恶意观看端攻击分享端，或反之）。
4. 分享端机器上的本地非特权进程（攻击 agent link、看护者 IPC、SYSTEM 服务）。
5. 浏览器观看端里的恶意网页/浏览器上下文。
6. 供应链（随 SDK 编译的 `third_party/`）。

**核心密码学结论（正向）**：临时 P-256 ECDH + HKDF，会话密钥绑定两条问候（含协议魔数）；设备密钥对“角色标签+此前全部握手内容”签名并用握手派生密钥加密（被动者看不到设备公钥）；双向独立密钥，AES-256-CTR 序号置于计数块高 64 位、每序号一用，再 HMAC-SHA256 截断 128 位先加密后认证；前向保密成立。详见第 4 节“确认稳健”。

## 3./4. 组件审阅与发现

### 3.1 握手与密钥协商（`src/transport/handshake.*`、`credentials.*`、`crypto/*`）

- **SA-01〔高〕PIN（CPace）对观看端的在线猜测没有计数，一次连接内可被试探数百次。**
  - 位置：`src/protocol/udp_connection.cpp:1868-1917`（客户端握手循环，对收到的**每个** `serverHello` 都调用 `client.finish()`），`src/transport/handshake.cpp:237-270`（`ClientHandshake::finish` 为 `const`，每次用同一 `pinScalar_` 重新对收到的服务器份额做一次 CPace 校验）。`include/rcsdk/rcsdk.h:1513-1527`（文档声称“观看端……每次连接试一次”）。
  - 攻击场景：攻击者处于分享端地址（伪分享端，或在途可向观看端发包的 MITM）。CPace 本身每次交换允许一次猜测，这没问题；问题是**一次观看端连接里**客户端会处理任意多个伪造 `ServerHello`，每个是一次独立 CPace 交换（不同服务器份额），只有猜中真实 PIN 的那个才会让 `finished` 校验通过、客户端据此发出自己的 `Finished`。客户端不在自己这侧计数（分享端的 `pin_attempts` 对此无效，因为被攻击的是观看端）。`refusalGrace`（1 秒，`udp_connection.cpp:73`）把窗口限制在首个失败应答后约 1 秒，但其内可塞入数百次。
  - 证据（已验证，`tests/security.cpp::viewerTestsManyPinGuesses`，环回）：一次连接内客户端试了 **352** 个伪造应答、用时 877 ms（约 401 次/秒），并用猜中 PIN `7351` 的那个完成了握手、与冒名者建立了会话。文档的“6 位 PIN + 5 次 = 1/200000”假设“每连接 1 次猜测”，实测约 400 次/连接，相差约 2–3 个数量级；4 位 PIN（允许的最小长度）在很少几次连接内即可被攻破。
  - 建议修复：观看端对**每个发出的问候只接受一个 `ServerHello`**（或极少数），其余丢弃并计入一个客户端侧的尝试计数；或在客户端也对 PIN 猜测计数并在超限后以 `AUTHENTICATION` 结束整个运行（不重连）。同时更新 rcsdk.h 的安全描述。
  - **状态**：已修（第二轮）：观看端每个问候只验证一个答复，猜错即以 `AUTHENTICATION` 结束运行（第 0 节）。

- **SA-04〔中〕未认证的 PIN 问候可把分享端的 PIN 认证“锁死”，拒绝真正的观看端（DoS）。**
  - 位置：`src/transport/handshake.cpp:298-304` + `credentials.cpp:36-43`（`PinAttempts::take` 在服务器**答复** PIN 问候时就占一个尝试），`udp_connection.cpp:1782-1816`（<16 个待定握手时新地址的问候不需要 cookie 即被答复）。
  - 攻击场景：未认证方（甚至伪造源地址，因未满 16 个待定时无需 cookie 往返）发若干带 PIN 份额、永不完成的问候；每个占一个尝试，失败累计达 `pin_attempts` 后，PIN 问候只回未认证短通知，真正持有正确 PIN 的观看端被拒，直到宿主重设身份。
  - 证据（已验证，`tests/security.cpp::pinLockedOutByHellos`）：5 个未完成的 PIN 问候后，持正确 PIN `246810` 的观看端被拒，错误为“The sharer accepts no more PIN attempts”。架构文档已提及达到 `pin_attempts` 后的短通知行为，但未认证即可触发锁死属 DoS。
  - 建议修复：只对**客户端已通过往返 cookie 证明其地址**的 PIN 握手计尝试；或把“未完成而过期”的尝试与“对端证明了却猜错”的失败分开计数，仅后者计入锁死阈值。
  - **状态**：已修：第一轮回答前就被拒绝的问候不再计失败；第二轮 UDP 上只回答用 Retry cookie 证明了地址的 PIN 问候（第 0 节）。

- **SA-09〔低〕UDP `ServerHello` 相对问候约 2.34 倍、可被反射。**
  - 位置：`udp_connection.cpp:1783-1799`（对与已存握手字节相同的重复问候，原样回同一 `ServerHello`，不触发新状态；对新地址的首个问候也在 <16 待定时直接回完整 `ServerHello`）。
  - 证据（已验证，`tests/security.cpp::repeatedHellosReflect`）：139 字节问候的 100 份副本各引出 325 字节应答，比值 2.34×。源地址伪造时构成 2.34× 反射放大（包数 1:1，倍率有限）。版本通知与 TCP 路径已刻意避免放大（见“确认稳健”），UDP `ServerHello` 是例外。
  - 建议修复：对未经 cookie 证明地址的来源，首个问候也只回一个 Retry cookie（像 ≥16 待定时那样），证明地址后再发 `ServerHello`。
  - **状态**：已修（第二轮）：问候补齐到和 `ServerHello` 一样大，比值 1.00×（第 0 节）。

- **SA-03〔低/参考〕已认证对端的 `inbound` 投递队列无显式上限。** **状态**：已修（第三轮）：最多 `maxInbound`（32 MiB），超过时可靠流不确认、等对端重发，画面、声音、不可靠数据按丢失处理并写日志（第 0 节“第三轮”）。
  - 位置：`udp_connection.cpp:615`（`inbound.push_back`，无容量限制）。乱序 `early` 有 `maxOutOfOrder=4096` 上限，可靠流由发送窗口与收到即确认约束；但按序交付的消息进入 `inbound` 等待会话 `receive()` 消费。若消费线程长时间停滞，已认证对端可令其无界增长。实际会话线程持续消费，难以触发，列为参考。

- **〔参考〕每条记录最多 64 MiB 的分配来自已认证对端。**`secure_channel.cpp:97,140`、`protocol::maxVideoBytes`。属已认证对端威胁、且一次只持一条，设计合理，仅记录。

### 3.2 认证加密记录通道（`src/transport/secure_channel.cpp`、`crypto/composed.cpp`）—— 确认稳健（现在只有助手链接用它）

- 记录头 4 字节长度 + 8 字节序号；`receive()` 校验 `0<size<=64MiB` 且 `sequence==receiveSequence_+1`（`secure_channel.cpp:134-155`），严格递增，重放/乱序/篡改被拒。
- AEAD：计数块把 64 位序号放高 8 字节、低 8 字节 0（`composed.cpp:52-67`），每序号一用、密钥流不重用；两方向不同密钥（`handshake.cpp:186-191`）。先加密后认证，HMAC 覆盖序号+头+密文。
- 常量时间比较 `crypto::equal`（`composed.cpp:29-38`）用于 `finished` 与 cookie 比较。
- 发送中断（`sendBroken_`）后拒绝复用序号（`secure_channel.cpp:102-110`），避免 nonce 复用。

### 3.3 密钥协商原语与 CPace —— 确认稳健

- ECDH 对对端公钥校验：系统后端 `SecKeyCreateWithData`/CNG 导入失败即 `AUTHENTICATION`（`crypto.cpp:337-378`）；BearSSL 后端点乘失败即拒（`crypto_bearssl.cpp:205-216`）；均要求 `0x04` 前缀、65 字节。测试覆盖非法点（`tests/crypto.cpp:103-115`）。
- CPace 低阶份额检查：`sharedSecret` 对中性元返回空（`cpace.cpp:122-128`），`handshake.cpp:95-98` 据此以 `AUTHENTICATION` 中止。
- 降级/秘密类型混淆防护：客户端要求 `serverHello[0]==credentials_.secret`，否则失败（`handshake.cpp:241-244`）；协议版本在握手魔数内，不匹配即 `UNSUPPORTED`（`handshake.cpp:42-64,199-205`），不与旧版本通信。
- 口令 PBKDF2（60 万轮）的“被伪造应答强制重算”防护：`costly()` + `derivationGap`（500 ms，每间隔只做一次昂贵派生）（`udp_connection.cpp:1884-1917`，测试 `tests/udp.cpp::forgedAnswersCostLittle`）。
- 握手洪水：UDP 用 cookie/Retry（`udp_connection.cpp:1766-1799`），TCP 最多 16 个并行、非阻塞推进、校验不过的 `finished` 只记下不改变状态（`connection.cpp`、`udp_connection.cpp:1724-1745`）。（现在 TCP 也走 `UdpAcceptor`，见 3.10。）

### 3.4 网络解析器与码流边界 —— 确认稳健（模糊测试佐证）

- `Reader`（`src/protocol/wire.hpp`）每次读都对边界检查，`end()` 拒绝尾随字节；`decode()`（`messages.cpp`）对每种消息逐字段做值域校验（编解码、图片格式、尺寸、输入事件、模块列表、数据分片长度等）。
- 码流在进任何解码器之前经 `videoProblem`/`vpx::packetProblem`/`av1::obus`/`nal::units` 的边界检查（`messages.cpp:447-490`、`media/av1.hpp`、`media/vpx.hpp`、`media/nal.hpp`）。
- 指针图像 RLE 解码边界精确（`media/cursor.cpp:97-121`），游程与 count 核对。
- STUN 属性解析逐个对缓冲边界核对、含填充步进（`transport/stun.cpp:194-212`）。
- ICE 信令 `rcs1.` 解码：长度前缀逐字段 `take()` 边界检查、候选上限 32、角色/类型/地址族校验（`transport/ice.cpp:163-237`）。
- **模糊测试结果**：对 `protocol::decode`、`decodeModuleList`、`agent::link::decode`、`stun::parse/parseCheck/verify*`、`ice::decode`、`av1::obus/codecConfiguration`、`vpx::*`、`nal::*`、`h264::noReorderingSps`、`file_transfer::decode*`、`clipboard::decode`、`web::Json` 共约 1.06×10^7 次执行，这些**二进制网络面解析器零崩溃**。唯一命中在浏览器 JSON（见 SA-02）。

### 3.5 浏览器（WebAssembly）包（`web/`）

- **SA-02〔中（对浏览器宿主）/低（整体）〕`web/json.hpp` 递归无深度上限（栈溢出）且浮点→整型越界是未定义行为。**
  - 位置：`web/json.hpp:146`（`read` 对数组/对象递归无深度限制）、`web/json.hpp:45`（`u32()` 对 `number_` 直接 `uint32_t(double)`，正向越界未判）。
  - 证据（模糊测试验证，ASan/UBSan）：`\x0a` + 大量 `[` → `AddressSanitizer: stack-overflow at json.hpp:146`；输入如 `[[...],7777...]` → `UBSan: 7.77778e+65 is outside the range of 'unsigned int' at json.hpp:45`。
  - 可达性：该 JSON 来自**宿主页面**（经 `rcsdk.mjs` 的 `rcsw_create`/`rcsw_update_request`/`rcsw_file_send`，`web/glue.cpp:565,816,972`），不来自网络对端。正常由 `JSON.stringify` 生成、良构；但若宿主把任何页面/服务器可控的值透传进配置，恶意或畸形输入会让 WASM 实例崩溃（页面自身 DoS）。影响局限在浏览器沙箱与该页面。
  - 建议修复：解析器加递归深度上限并在超限时返回 null；`u32()/u64()` 对越界 double 做饱和/取 0。即便输入来自宿主，解析器也应对畸形鲁棒（纵深防御）。
  - **状态**：已修，见第 0 节。
- **SA-14〔低〕浏览器观看端永久保留每个指针形状，恶意分享端可耗尽标签页内存。**`src/session/cursor_stream.cpp:133-139`（每个新 `shape_id` 只上报一次）、`include/rcsdk/rcsdk.h:2393-2394`（“宿主保留收到的图像”）、浏览器侧 `web/glue.cpp:337-343`（每形状最多 256×256×4 字节）、`web/pre.js:117-135`（逐像素 JS + `toDataURL()`）、`web/rcsdk.mjs:182`（`_cursorShapes` Map 从不清理）。分享端用新 id 连发 256×256 噪声形状，每个在主线程留数百 KB PNG data URL，直到标签页崩溃；id 仅校验非零（`protocol/messages.cpp:405`）。修复：页面 Map 设 LRU 上限；或改契约让核心在驱逐后重发形状（对齐 `heldImages=32`）而非由宿主全保留；限速。 **状态**：已修：宿主只需保留最近 32 张，会话会重发，见第 0 节。
- **SA-15〔低〕密钥/PIN/口令留在 WASM 堆（SharedArrayBuffer，同源脚本可读），且指南缺少设计承诺的加固建议。**`web/rcsdk.mjs:115-133`（经 `stringToNewUTF8` 把十六进制密钥/PIN/口令放入堆中的 JSON，`_free` 前不清零）、`web/glue.cpp:565-586`（秘密拷进 Json 树与栈上 `config.identity`，均不擦除；核心也不擦 `identity`）。整个堆是 SharedArrayBuffer，AudioWorklet 也拿到 `HEAPU8.buffer`（`pre.js:52,87`）。前提：页面内有脚本（XSS 或第三方脚本）——即便页面已丢弃自己的副本仍可恢复秘密。浏览器观看端的设计称指南会告知宿主“无第三方脚本、严格 CSP”，但当时的指南 23 无此内容。修复：`_free` 前清零（`HEAPU8.fill(0,p,p+len)`）；密钥以字节而非 JSON 十六进制传入；擦除 Json 与 config 副本；把 CSP/禁第三方脚本写进指南。 **状态**：已修，见第 0 节。
- 〔参考，F4〕浏览器 canvas 按解码帧的显示尺寸设大小而不校验（`web/pre.js:278-281`，`src/platform/web/decoder.cpp:128-129`、`view.cpp:570-608` 未与公告尺寸/`maxPicture` 比较）；且结果按顺序与包配对（`decoder.cpp:108-123`），VP9 超帧/AV1 时间单元产生的多帧可能堆积未关闭的 VideoFrame。未在浏览器实跑验证。修复：缩放前核对解码尺寸与公告尺寸/`maxPicture`；关闭多余结果。 **状态**：已修（第三轮，没在浏览器里跑）：画面必须是包说的尺寸，否则以 `CODEC` 拒绝；按块编号配对，多余的画面一到就关掉（第 0 节）。
- 〔参考，F14〕宿主启用 `clipboard.receive` 时，`web/rcsdk.mjs:777-781` 在页面下一次点击/按键时把分享端的剪贴板写入系统剪贴板（浏览器会清洗 HTML/图像），恶意分享端可反复替换观看端剪贴板；`readOnFocus` 则每次聚焦把本地剪贴板发给分享端。建议文档化。 **状态**：已写进指南 23（第 0 节）。
- **已确认稳健（浏览器）**：传输由 SDK 自己的握手与 AEAD 端到端加密，不依赖 WebSocket 的 TLS（`session/connect.cpp:151-163`）；`load()` 要求页面跨源隔离（COOP same-origin + COEP require-corp，`rcsdk.mjs:51-54`，指南 23）；Worker 仅经 MessageChannel 端口/专用 Worker 消息到达，无 window `message` 监听器，故不需源检查（`pre.js:35-41,619-623`）；事件 JSON 由 `JsonWriter::quoted` 转义（`json.hpp:263-278`）、`JSON.parse` 解析，>16 字节字符串用 TextDecoder，故对端的非法 UTF-8 不能破坏 JSON；对端文件名是严格 UTF-8 且做组件校验（`modules/file_transfer/src/paths.cpp`、`common.cpp:80-117`），File System Access API 也拒绝 `..`；WebCodecs 配置构造用有界 `snprintf`、其解析器对截断抛错（`media/web_codec_config.hpp`、`sampling.hpp:41-73`、`av1.hpp`、`vpx.hpp:250-264`）；宿主通道分帧上限 8 MiB（`host_channel.cpp:21-63`）。

### 3.6 助手链接、看护者、输入控制（专项审阅已完成，结论见下）

本组通过并行代码审阅完成（未写 harness、未在真机验证，Windows 部分仅阅读）：

- **SA-05〔中〕F1 Unix agent 端点：`bind` 后用路径 `chmod` 设权限（跟随符号链接），且不检查端点所在目录归属。**`src/transport/local_stream.cpp:576-589`（lstat→unlink→bind→`chmod`）、`:519-545`（锁文件）。前提：会话以 root 运行（文档的 macOS LaunchDaemon）且他人可写端点目录或其上级；可被在 bind 与 chmod 间换路径，使 root 改到本不该动的文件的权限（ANY_USER 下 0666，可致本地提权）；粘滞共享目录（如 `/tmp`）下降级为 DoS。锁文件 `O_CREAT|O_NOFOLLOW` 无 `O_EXCL`、无 `S_ISREG/st_nlink==1` 检查。修复：绑定前用 `O_DIRECTORY|O_NOFOLLOW` fstat 核对父目录归属与非组/他人可写；用 `fchmodat(AT_SYMLINK_NOFOLLOW)`/`lchmod` 或 `mkdtemp`+`rename`；设权后再 `lstat` 核 `S_ISSOCK` 与 uid；文档要求端点放仅会话账户可写的目录。 **状态**：已修，见第 0 节。
- **SA-06〔中〕F2 WINDOW/APPLICATION 限定没有覆盖键盘，与文档承诺矛盾。**文档 `include/rcsdk/rcsdk.h:862-869`、`docs/guide/07-sharing.md:50` 称键入只在共享应用有焦点时执行。但 (a) 媒体/消费键跳过焦点规则：Windows `windows/input.cpp:191-205`（含 VK_LAUNCH_MAIL/BROWSER_SEARCH/HOME，`src/platform/keys.hpp:147-151`），macOS `macos/input.mm:211-221`；(b) 焦点检查只在键按下且只对焦点**进程**（`input.cpp:178`、`input.mm:172`），修饰键组成的全局快捷键（Win/Cmd 组合、Alt/Cmd+Tab、Cmd+Space）在共享应用在前时可达（真机未验证各组合效果）；(c) WINDOW 下键达同进程任意窗口，指针按下接受该进程任何有属主或 `WS_POPUP` 的窗口（`input.cpp:259-265`，未核对属主是否即共享窗口）。修复：限定时拒绝消费/辅助键与 GUI 键，或给宿主策略；WINDOW 要求 `GetAncestor(front,GA_ROOTOWNER)==window_`；并改正 rcsdk.h 与 guide 措辞，不把限定当作安全边界。 **状态**：已修，有写明的残余，见第 0 节。
- **SA-07〔低〕F3 macOS 助手以 PID 识别，而链接密钥对所有用户可读。**`local_stream.cpp:502-511`（getpeereid+`LOCAL_PEERPID`）。文档设置使 `link.key` 0644 全可读（`examples/unattended/macos.cpp:226`）、用 ANY_USER。PID 命名进程不命名持 socket 的代码（exec/传描述符/PID 复用可变）。控制台用户下的进程可冒充助手，向观看端供伪造画面、收远程输入。修复：accept 时取 `LOCAL_PEERTOKEN` 审计令牌，用 `SecCodeCopyGuestWithAttributes` 做代码签名核验并经 `rcs_agent_info`/`rcsdk/apple.h` 交给宿主；Linux 用 `SO_PEERPIDFD`；文档说明 `screen_permission` 系自报。（Windows 侧因看护者持有 helper 进程句柄，PID 不会被复用，该门成立。） **状态**：已修，见第 0 节。
- **SA-08〔低〕F4 Ctrl+Alt+Del 中转信任客户端 PID，且单实例可被占用致 DoS。**`components/supervisor/src/supervisor_windows.cpp:256,149,249,274`。同账户任一进程可连并长期占用管道，阻断远端 CAD；PID 在请求后查，客户端退出且 PID 复用的窄窗口内 SAS 可发到复用 PID 的会话。修复：只服务 `client==helper.processId`（看护者持句柄，PID 不会复用），或比对 `GetNamedPipeClientSessionId` 与令牌会话 ID。 **状态**：第二轮已修（只交叉编译）：每个被看护进程一条只连着它继承的那一头的管道，按看护者持有的进程句柄的令牌发，见第 0 节。
- **SA-10〔低〕F5 端点名抢占致会话无法启动（仅 DoS）。**Windows `FILE_FLAG_FIRST_PIPE_INSTANCE`（`local_stream.cpp:321`）与 Unix 共享目录。助手仍拒绝与抢占者通话（默认 PRIVILEGED），不能被接管。修复：看护者每次运行给 helper 随机名（已对 SAS 中转名这样做）。 **状态**：已修报错，见第 0 节。
- **F6〔参考〕** 同账户进程可反复失败+拖延使助手一直等待（`agent/remote.cpp:146-160,206-217`）——该账户本可停自己的助手。
- **F7〔参考〕** CAD 无独立开关：任何有控制权的观看端都可发（`share_agent.hpp:146-148`），仅观看（无控制）者不能；宿主可用 `on_input` 否决或 `host_input_types` 自接。建议文档化或加独立开关。 **状态**：写明（第三轮）：`on_input` 和 `host_input_types` 已是开关，不另加字段（第 0 节）。
- **F8〔参考〕** 看护者接受 UNC 程序路径（`api.cpp:120`），可从网络共享以 SYSTEM 运行。建议拒绝或文档化。 **状态**：已修（第三轮）：共享和设备路径、映射到共享的盘符都是 `INVALID_ARGUMENT`。
- **F9〔参考〕** `secure_desktop` 关时，桌面切换与下次轮询（1 秒，`agent/media_agent.cpp:258`）之间仍发输入，依赖 Windows 拒绝非输入桌面线程的 SendInput；建议注入前核对输入桌面名。 **状态**：已修（第三轮，只交叉编译）：每个事件执行前核对输入桌面是用户自己的。

**本组确认稳健**（代码审阅）：Windows 命名管道受保护 DACL（SYSTEM/管理员/会话账户；ANY_USER 加 AU 但不给创建实例权）、`FIRST_PIPE_INSTANCE`、`REJECT_REMOTE_CLIENTS`、客户端 `SECURITY_IDENTIFICATION` 且无 GENERIC_WRITE（`local_stream.cpp:321-344,401-403`）；链接顺序“系统报告的对端→12 字节明文前缀→安全通道→hello”，通道起来前不解析其他（`agent_server.cpp:327`、`link_messages.cpp:583-607`）；全零链接密钥两侧皆拒（`config.cpp:424-425`、`c_api.cpp:1222-1225`）；finished MAC 常量时间比较；链接消息逐字段校验且助手送来的包再经网络同款检查（`link_messages.cpp` 多处，`remote.cpp:355-358`）；看护者启动用 `lpApplicationName` 分离、句柄清单限定继承、密钥走匿名管道不入命令行/环境、helper 挂 kill-on-close job、变量仅当真父进程匹配才采用（`supervisor_windows.cpp:589-623`、`helper.cpp:27-68`）；SAS 管道随机名/单实例/保护 DACL、请求须恰为 1 字节 `'S'`、系统策略只读、`sas.dll` 仅从 System32 加载（`supervisor_windows.cpp:141-190,249-250`、`desktop.cpp:111,156-177`）；**控制授权门**：epoch 与授权在 `control.mutex` 下于 `on_input` 前、注入器运行前各查一次，撤销/目标变更/不可用/离开时释放所有按键，释放与排队输入同队有序（`share_viewer.cpp:246-276`、`session.cpp:474-494`、`share.cpp:704-719,862-875`、`remote.cpp:533-548`、`input.cpp:69`/`input.mm:58-59`）；准入前只接受 Admission（`share.cpp:1563-1568`）；输入校验 `validInput`（`messages.cpp:516-540`）在助手侧再校验一次（`link_messages.cpp:877-884`）。

### 3.7 模块与官方模块（文件传输、剪贴板、缩略图、文本插入点、电源）

- 模块消息路由与装配：`DataReceiver::part`（`src/session/data_channel.cpp:226-278`）按序、一次一条，`limit_` 按模块/种类设上限，越限即 `PROTOCOL` 断连；模块列表 `decodeModuleList`（`messages.cpp` 含 `RCS_MODULES_MAX`、id 校验、重复拒绝、hello 上限）。
- 解析器模糊测试：`file_transfer::decodeHello/decodePeer/decodeRequest/decodeReply`、`clipboard::decode` 在上述 1.06×10^7 次执行中**零崩溃**。
- **SA-11〔中〕文件传输 4 MiB 流控窗口可被对端重发 OFFER 任意重置，对端驱动的写入随后无界排队（已认证对端 → 接收进程内存耗尽）。**
  - 位置：`modules/file_transfer/src/common.hpp:38`（`windowBytes = 4u << 20`，“一次一个传输在途的量”）、`session_half.cpp:939`（发送侧按 `outstanding<windowBytes && held()<windowBytes` 节流）、`session_half.cpp:1386`（重发 OFFER 时“发送方窗口从此重新开始”）。
  - 攻击场景（来自模块的专项审阅，代码阅读、未写 PoC）：接收宿主已接受某传输后，已认证对端反复重发 OFFER 重置窗口，使写入在磁盘慢于网络时无界排队，内存耗尽上限为已接受的传输大小（观看端打分享端或反之）。使用远程助手（`rcs_agent`）时则改为强制断开助手链接。
  - 建议修复：重发 OFFER 不应重置在途/已排队计数；按已持久化的进度而非重置窗口来续传，并对接收侧未落盘的缓冲设独立上限。
  - **状态**：第二轮已修：两端交给存储的读写在存储回答前一直算数，接收方等存储写完才回新的 `ANSWER`，发送方的同类问题和条目失败的绕过一并修了；测试 `file_transfer.flow` 验证（34 MiB → 4.25 MiB，128 → 16 个读），见第 0 节。
- **SA-16〔低〕文件传输把本机绝对路径发给对端，并在日志里记录（向已认证对端泄露用户名与目录结构）。**路径来自 store 错误：`modules/file_transfer/src/store.cpp:302,339,345,360,376`（`files::join(directory, spec.path)`）、`files_posix.cpp:22-23`（“Cannot open /home/u/…”）；发给对端：`session_half.cpp:1507,1528`（`settle(...,tellPeer=true)` 发 `ItemDone.message`）、`:984`（`ItemEnd.message`）；日志 WARNING：`machine_half.cpp:28`。修复：只把相对项路径+结果码发给对端，完整文本留本地。 **状态**：第二轮已修：原因一律用条目在线上的相对路径（本地和对端看到的是同一句），日志不写目录，见第 0 节。
- 对端文件名在浏览器侧经严格 UTF-8 与组件校验（`modules/file_transfer/src/paths.cpp`、`common.cpp:80-117`，File System Access API 亦拒 `..`）——此路径已确认。
- **本轮未充分覆盖（列入残余风险，优先后续）**：原生（非浏览器）文件传输的**符号链接/junction 的 TOCTOU、覆盖规则、断点续传令牌、磁盘耗尽、创建文件权限**（`files_posix.cpp`、`files_windows.cpp`、`store.cpp`）；剪贴板的**隐藏/瞬态数据标记是否被尊重、图像/HTML/RTF/文件列表解码与大小、未经用户动作注入本地剪贴板、方向规则**（`modules/clipboard/src/content.cpp`、`system_*.{cpp,mm}`、`session_half.cpp`）。解析入口字节级鲁棒且文件名组件校验已确认，但上述业务规则需后续逐行审与定向测试。**第二轮**：原生文件传输的这些规则和剪贴板的隐藏 / 瞬态标记已逐行审阅，错的已修（第 0 节“第二轮的审阅结论”）；剪贴板的图像 / HTML / RTF / 文件列表解码、方向规则、无用户动作注入仍待审。**第三轮**：已逐行审阅，文件名规则、预取按列出的查、macOS 大图不转 TIFF 三处已修（第 0 节“第三轮的审阅结论”）。

### 3.8 STUN/TURN/ICE 与 TLS to TURN（专项审阅已完成，含 ASan/UBSan harness：2M STUN、500k TURN 数据报、300k 信令，无内存/UB 错误）

- **SA-12〔低（宿主若中转未认证方的信令则中）〕ICE 接受信令里的任意候选地址，可令 SDK 向 loopback/内网/云元数据/组播地址发 STUN 探测（类 SSRF）并放大约 20×。**
  - 位置：`src/transport/ice.cpp:210-230`（decode 只校验类型、4/16 字节长度、端口≠0）、`:596-617`（`use()` 配对每个候选）、`:623-644`（`sendCheck`）；并发 `src/session/signals.hpp:32-33`（每分享端 32 并发 + 256 等待）。
  - 证据（harness 验证）：含 `127.0.0.1:6379`、`255.255.255.255:53`、`224.0.0.251:5353`、`0.0.0.0:80`、`169.254.169.254:80`、`[::1]:22` 的信令都能解码并被接受。一条约 1.4 KB 信令使目标发出约 32 对 × 7 次（`maxAttempts`，`ice.cpp:29`）≈ 224 个 92–152 字节 Binding 请求，约 20× 放大；USERNAME 含攻击者可控的十六进制 ufrag。探测是盲的（回答需 MESSAGE-INTEGRITY）。RELAY 模式下 `sendTo` 调 `turn->open(to)`（`ice.cpp:355-358`），向 TURN 请求到攻击者选定对端的权限/信道，仅服务器的拒绝对端配置能拦。非崩溃向量（UDP 发送错误被忽略，`socket.cpp:418-428`）。
  - 建议修复：加一个通用能力（候选过滤回调或地址类别掩码），默认丢弃未指定/组播/广播地址；对信令加每路速率限制。
  - **状态**：已修（第二轮）：按地址范围剔除候选、同一 IP 最多 4 个，`ice_addresses` 可给出本机环回地址（第 0 节）。
- **SA-13〔低〕Android 上 BearSSL 信任用户添加的 CA 证书，忽略宿主应用的网络安全策略。**`src/transport/tls_bearssl.cpp:599-601` 读 `/data/misc/user/<id>/cacerts-added` 进信任锚。面向 API 24+ 的应用默认不信任用户 CA，除非其 network security config 显式允许。攻击者=网络 MITM + 设备上装了 CA（MDM 或被诱导）。影响：TURN-over-TLS 的 MITM（看到 TURN 用户名与 HMAC，够离线猜口令；看到元数据；可拒服），媒体仍端到端加密。修复：默认不含用户 CA，或加宿主开关；文档说明 pin 为强选项。 **状态**：已修，见第 0 节。
- **〔参考，F9〕证书校验细节**：BearSSL “minimal” 引擎忽略非关键 EKU、无 SAN 时回退到 CN（`third_party/bearssl/inc/bearssl_x509.h:80-96`，用于 `tls_bearssl.cpp:176-181`）；三个后端均无吊销检查（Windows `platform/windows/tls.cpp:386-397` 无吊销标志，Apple `tls_connection.mm:319` `SecTrustSetNetworkFetchAllowed(false)`，BearSSL 无）；无宿主 CA bundle 选项，只有叶证书 SHA-256 pin（私有 CA 的宿主每次续期需重新 pin）。 **状态**：第三轮：三个后端都拒绝扩展密钥用法不含服务器认证的、没有 subjectAltName 的证书（BearSSL 和 Windows 原来接受）；吊销与 CA 包不改，理由见第 0 节。
- **〔参考，F7〕** `ice::decode` 先 base64 解码任意大小再校验（`ice.cpp:163-173`，入口 `session.cpp:1443`）：50 MB `"rcs1.AAAA…"` 会被完整解码后才以“未知版本”拒绝。修复：先拒绝超过约 1380 字符（最大合法信号 1030 字节 = 1374 字符加前缀）。 **状态**：已修（第二轮）：上限按最长的重启信令算（1060 字节，1419 个字符）。
- **〔参考，F8〕** TURN 长期凭据（RFC 8656 标准行为，列全）：未认证错误被接受（`turn.cpp:296-321,174-191`），在途攻击者见事务 ID 可用 4xx 终止中继或用 438 打环；服务器从不发 401 则 realm 为空、无完整性，在途者可用自己的 XOR-RELAYED-ADDRESS 应答 Allocate（`turn.cpp:59-62,193-207`）；LIFETIME 未钳制，0/1 使每拍都刷新（`turn.cpp:202,330-331`）。影响仅 DoS/流量分析，数据仍端到端加密。修复：LIFETIME 钳到至少 60 s；可选在配了 TURN 凭据时拒绝未认证的 Allocate 成功。 **状态**：已修（第二轮）：刷新至少隔一秒、438 只重发一次、配了用户名时不接受未要凭据的分配；认证前的 4xx 照旧（第 0 节）。
- **已确认稳健（STUN/ICE/TURN/TLS，harness + 代码审阅）**：
  - STUN：精确长度/cookie/高位检查（`stun.cpp:176-179`）、逐属性边界（`:194-201`）、FINGERPRINT 必须最后且验证（`:202-204`）、MESSAGE-INTEGRITY 之后属性丢弃（`:205-209`）、XOR 前校验地址族与长度使掩码下标 ≤15（`:112-127`）、完整性常量时间比较（`:223-232`、`composed.cpp:29-37`）。
  - ICE：凭据 32 位 ufrag / 128 位口令（`ice.cpp:308`），decode 强制十六进制、长度 4..64/22..128、至多 32 候选、无尾随字节（`:163-235`）；响应须同事务、用对端口令验完整性、源与 socket 对称（`:727-754`）；`parseCheck` 须 Binding 请求 + FINGERPRINT + `localUfrag:` 前缀 + 完整性（`stun.cpp:242-250`），对端自反学习上限 32（`ice.cpp:704-716`），选路后仍答 check 的链路需完整性（`udp_connection.cpp:187,224`）；信令队列有界。
  - TURN：ChannelData 长度校验（`turn.cpp:271-283`），已知 realm 后成功响应需完整性（`:194,304`），401 只学一次（`:101`），信道上限 0x7FFF；TCP 分帧至多一个 65555 字节帧于 128 KiB 缓冲、超大丢弃（`turn_tcp_socket.cpp:96-123,200-223`）；UDP 发送错误从不中止会话（`socket.cpp:418-442`）。
  - TLS 建立：配置拒绝无 pin 且无 DNS `turn_tls_server_name` 的 IP 地址 `turn_server`（`config.cpp:432-470`），故无 pin 连接必有名字；`tlsSettings` 只从 DNS 主机派生名字（`tls.cpp:11-26`）。
  - BearSSL：仅 TLS 1.2、ECDHE+AEAD、无重协商、SNI + 名字校验（`tls_bearssl.cpp:43-46,298-321`），时间取 `time()`，pin 模式对叶做 SHA-256 并用 pin 证书的密钥做握手签名（`:251-267`），无锚时失败关闭，测试覆盖名字/过期/不信任 CA/pin（`tests/tls.cpp:188-206`）。
  - Apple：verify block 取代默认检查但用 `SecPolicyCreateSSL(true,name)` 评估（`tls_connection.mm:305-339,359-371`），关会话恢复（`:355`），TLS≥1.2，拒绝转 AUTHENTICATION。
  - Windows：手动校验在握手后、应用数据前运行（`platform/windows/tls.cpp:180,366-384`），要求 serverAuth EKU + 带服务器名的 SSL 策略（`:388-419`），重协商后复查（`:498`），仅 TLS 1.2/1.3 强算法，`SEC_I_INCOMPLETE_CREDENTIALS` 等失败关闭。

### 3.9 转发器（`server/forwarder/`、`RCS_TRANSPORT_FORWARDER`）

**范围**：转发器库与程序（`server/forwarder/src/forwarder.cpp`、`entry.cpp`、`token.cpp`、`api.cpp`、`main.cpp`），SDK 两端（`src/session/forwarder.*`、`src/protocol/media_keys.*`、`src/protocol/messages.*` 里转发器那一跳的消息、`src/protocol/route_demux.*`），公网入口用到的 `src/transport/websocket.*`（HTTP/1.1 升级和 RFC 6455 的帧，两端共用）、`framed_socket.*`（字节流上的数据报）、`crypto::sha1`（只算 `Sec-WebSocket-Accept`）和 TLS 客户端的 ALPN、核对设置（`tls.*` 与三个后端），用法见[指南 24](guide/24-forwarder.md)。那一跳也能走宿主给的通道（`RCS_FORWARDER_HOST`，`src/session/forwarder.cpp` 的 `hopPath`、`session.cpp`），浏览器包（`web/glue.cpp`、`pre.js`、`rcsdk.mjs`）用页面的 WebSocket 连 WebSocket 入口，示例 `examples/web/serve.mjs --forwarder` 替页面签令牌、把 `/forwarder` 转给入口。转发器是新的信任边界：宿主部署在服务器上，所有端都向它发起连接（UDP、TCP 或 WebSocket；`wss://` 由它前面的代理终结 TLS），它为一个房间里的每个观看端复制分享端的画面和声音，并为晚来的人留着最近的关键帧。

**信任假设**：只信转发器做转发（带宽、复制、调度、每个观看端的重传和码率），不信它看内容、不信它说一个人是谁。

**威胁与措施**：

| 威胁 | 措施（位置） | 结论 |
| --- | --- | --- |
| 被攻破或恶意的转发器看画面、声音、输入、数据 | 画面每张、分享端的声音每包用流密钥做一次认证加密（AES-256-CTR + HMAC-SHA256-128，`media_keys.cpp`），头（消息类型、路、档、层、关键帧、可丢弃、声音编号、密钥编号、计数器）作为附加数据；流密钥只经各观看端自己的端到端连接（`MediaKey`）发给分享端放行的人；其余一切走端到端连接，转发器只原样透传数据报（`Pipe`） | 转发器拿不到明文和密钥 |
| 转发器改画面、重放、换头、拼接 | 观看端核对认证、头与内容一致（路、档、层、标志）、每把密钥 4096 的重放窗口；一把密钥一个计数器，覆盖所有路和档，(密钥, 计数器) 不重用 | 改了的、重复的被拒并计数（`media_rejected`、`media_replayed`），解码从下一个关键帧继续 |
| 转发器把录下的密文给后来的人或离开的人 | 每个关键帧密钥 HKDF 推一步（推不回去）；放行一个人时分享端给它所要的几路最近关键帧所在那一步的密钥，只在转发器说它还留着（`cache_ms` 以内）时，否则给往后一步；有人离开、宿主请走人、`key_rotation_ms` 到了时换一代随机密钥，只发给留下的人 | 新人解不开它来之前超过 `cache_ms`（转发器的设置，默认 3 秒，0.1..60 秒）封装的（FW-7）；分享端的 `from_admission` 设 1 时一点也解不开；离开的人解不开新一代启用之后的（启用前最多 1 秒，见残余风险） |
| 转发器冒充别人的路由号 | `rcs_viewer_info.route` 经转发器时写明只是转发器转述的提示；按人授权要用端到端的凭据 | 写进头文件和指南 |
| 转发器伪造很多观看端（`PeerJoined` 加管道数据）压垮分享端 | 分享端的管道是 `RouteDemux`：等握手的一个 peer 64 KiB、合计 1 MiB、最多 1024 个 peer，`UdpAcceptor` 同时 16 个握手并要 Retry cookie；记住的路由号最多 1024 个；每个都要通过端到端握手和宿主的准入 | 有界；伪造的 peer 连不过握手 |
| 冒充转发器（中间人） | 握手里转发器用自己的 P-256 身份签名，端点按 `forwarder_key_sha256` 核对，对不上就以 `AUTHENTICATION` 失败、令牌不发出；经 TCP、WebSocket、TLS 代理也一样（那一跳的握手在它们里面） | 令牌只给真的转发器；TLS 代理、做 TLS 检查的中间盒看得到的只是那一跳的密文 |
| `wss://` 的 TLS | 转发器不终结 TLS：代理终结，端点用系统的 TLS 客户端，默认核对证书链和 URL 的主机名，或钉住代理证书的 SHA-256（`tls_certificate_sha256`）；配置拒绝没有钉住而主机是 IP 的 `wss://`、`ws://` 加钉住；核对失败以 `AUTHENTICATION` 失败、不回退到 `ws://` | TLS 只为穿过只放行 443 的网络；就算 TLS 被破，那一跳仍认证转发器、端到端仍加密 |
| 没有令牌、伪造令牌、偷来的过期令牌、给别的转发器的令牌 | HMAC-SHA256 常量时间比较，固定布局（`rcsf1`）、严格 base64url（不规范的编码也拒），版本、长度、角色、房间检查，过期按 Unix 时间，受众（转发器公钥的 SHA-256）必须是这一台；两把密钥按 `key_id` 并存；或者宿主自己核对（`verify_token`，受众由宿主查）。拒绝时只说哪一项不对，不泄露密钥 | `REFUSED` |
| 订阅令牌用来发布、第二个发布者顶替 | 角色必须和令牌一致；一个房间只有一个发布者，第二个 `REFUSED` | 不能挤掉分享端 |
| 握手洪水、伪造源地址反射 | 和 UDP 端口相同的 `UdpAcceptor`：16 个并行握手、Retry cookie、过期报告 | 继承 SA-09 的状况（见 FW-6） |
| 握手后不说话、大量占位 | 未报到的端最多 32 个、5 秒内不发令牌就断开（`hello_timeouts`），超过的立即 `BUSY` | 有界 |
| 管道洪水（观看端灌分享端，或反过来） | 每条管道每个方向一个令牌桶（`max_pipe_bps`，默认 50 Mbit/s，每个数据报至少计 256 字节），超出丢弃并计数（`pipe_dropped`） | 有界 |
| 超大或畸形的消息 | 每种消息逐字段校验（令牌最多 1023 字节、管道数据报最多 65535 字节、加密画面和声音有大小上限、声音编号非 0、路径报告最多 24 条）；违反规则的端（例如观看端发只有分享端发的消息）被断开并计数（`rejected`） | 断开该端，房间照常 |
| WebSocket 入口的 HTTP 解析 | 开头至多 8 KiB、64 行头，不收折行、空的或带空格的头名、控制字符、不是路径的目标、别的版本、不是 16 字节的 base64 键（0x80 以上的字节在字段值里照 HTTP 的 obs-text 收下：浏览器会带上站点的 UTF-8 Cookie）；没升级的回 426、不是 GET 的 405、读不懂的 400、太长的 431，然后关掉。开头到空行为止至多缓存 8 KiB，之后才逐行解析 | 有界；`rcsdk.websocket` 逐项测过，`rcsdk.forwarder` 对真入口测过，模糊测试见 FW-9 |
| WebSocket 帧 | 客户端的帧必须掩码；保留位、未知操作码、长度不是最短形式、控制帧超过 125 字节或分片、续帧没有开头、在消息中间开新消息：关闭码 1002；文本消息 1003；一条消息（不管几帧）超过 65535 字节：1009，在读到负载之前就判；ping 答 pong，close 答 close；协商不出任何扩展（不压缩，没有解压炸弹） | 关掉该连接、计数（`connections_refused`），房间照常 |
| 慢速攻击（slowloris）、占着连接不用 | 开头 5 秒内到齐；10 秒内连接上没完成那一跳的握手就关；每条连接每轮最多读 4 次（别的连接也轮得到） | 有界 |
| 连接洪水 | 每个 TCP、WebSocket 入口同时至多 `max_ends` 条连接，多一条立即关掉并计数；握手照旧每个入口 16 个、Retry cookie | 有界，满了的连接不进房间 |
| 慢读的端撑大发送队列 | 每条连接等着写出的至多 256 KiB，多的丢掉（像满了的 UDP 套接字），那一跳自己重传、降码率；关闭中的连接 1 秒内写不完就断 | 有界 |
| 分享端灌转发器 | 分享端的画面和声音一起按 `max_publish_bitrate_bps`（默认 400 Mbit/s）的令牌桶，多的丢掉计数（`publish_dropped`）；令牌的 `max_bitrate` 另限观看端 | 有界 |
| 缓存撑大内存 | 每个房间每一路每一档只留最近一个关键帧之后的，`cache_ms`、每个房间 `cache_bytes`（默认 16 MiB）、全部房间 `max_cache_bytes`（默认 256 MiB），超了就不留直到下一个关键帧；分享端到转发器的路上丢了画面，留着的作废 | 有界；`cache_bytes` 统计 |
| 观看端积压撑大转发器内存 | 每个观看端那一跳的视频积压由连接自己的上限丢弃（`maxQueuedVideo`），转发器随即把它移到低档或等关键帧 | 有界 |
| 房间、观看端、端的数量 | `max_rooms`（默认 64）、`max_viewers_per_room`（默认 64）、`max_ends`（默认 1024，所有入口一起） | 满了 `BUSY`，写明哪一项 |
| 宿主通道上的那一跳（浏览器观看端） | 那一跳的握手、转发器身份的核对、令牌、端到端连接和流密钥都和别的链路一样，只是数据报经宿主的通道；通道（页面的 WebSocket、宿主的中转）能丢、拖、乱序、换到别的转发器，都落在同样的检查上（换了转发器以 `AUTHENTICATION` 失败、令牌不发出）；宿主通道上数据报至多 1200 字节，`host_max_datagram` 小于 1200 的配置被拒；一条到入口的连接就是一个那一跳，断了按断线重连 | 不比 UDP、TCP、WebSocket 链路多信任什么；`forwarder_host` 测过 |
| 浏览器页面里的令牌、跨站的 WebSocket | 令牌由页面在请求体里向宿主的服务器要、经 `createSession` 交给模块，选项的文本在模块内存里用完清零（会话留着一份重连用，和连接密钥一样）；示例的 `serve.mjs` 只给自己的页面（`Origin`）签、令牌密钥只从文件读、`/forwarder` 只转到它被告知的转发器。转发器不看 `Origin`：WebSocket 入口上没有 Cookie 那样随请求自动带的凭据，别的网站的页面连得上入口也没有令牌、过不了端到端握手 | 页面里的脚本仍读得到令牌和流密钥（指南 23 的 CSP 建议照旧）；宿主要只让自己的网站连，可以在代理上按 `Origin` 拦 |
| 转发器被攻破（例如入口的解析器出错）后在服务器上做别的（Linux 服务） | 部署样例 `deploy/rcs-forwarder.service`：专用账户（`useradd --system`，不能登录），`NoNewPrivileges`，能力全无（`CapabilityBoundingSet=` 空；只有不放代理、直接听 1024 以下端口时由宿主的 drop-in 加 `CAP_NET_BIND_SERVICE` 一个），`ProtectSystem=strict`（文件系统只读，只有 `/var/log/rcs-forwarder` 可写）、`ProtectHome`、`PrivateTmp`、`PrivateDevices`、`ProtectProc=invisible`，只许 IPv4、IPv6、Netlink 套接字，系统调用 `@system-service` 去掉 `@privileged`、`@resources`，`MemoryDenyWriteExecute`、`RestrictNamespaces`、`RestrictSUIDSGID`；配置和身份私钥属于 root、只有它的账户读得到（目录 0750、文件 0640），日志 0640；容器里以 990 号账户运行。示例配置的令牌密钥是占位（若是全零的密钥，忘了换就能被任何人签令牌），不换就不启动 | 攻破后拿得到的只是转发器本来就有的：令牌密钥、身份私钥和流经的密文（内容仍是端到端加密的，见上）；改不了系统、读不到别的账户。Ubuntu 24.04 arm64 虚拟机上实测：进程的能力全是 0、`NoNewPrivs` 1、seccomp 过滤开着，`systemd-analyze security` 1.3（“OK”，剩下的主要是联网本身）；不加能力听 853 端口启动失败（`permission denied`），drop-in 加了之后进程只有这一个能力 |
| 弱观看端让转发器频繁要关键帧 | 没有更低档可去、积压远远落后的观看端，转发器丢掉它的积压、从留着的或要来的关键帧重新开始，每个观看端至多每 `restartWait`（2 秒）要一次；被移到低档的人每次移档要一次 | 一个弱观看端给同一档的别人添的关键帧不比直连时（`viewer_keyframe_interval_ms` 默认 2 秒）多；`rcsdk.forwarder` 的 `behindAlone` 测过 |

**测试**（Mac 本机、只用环回；全部通过）：

- `rcsdk.forwarder`（`tests/forwarder.cpp`）：令牌签发与核对、过期、密钥编号不认识、签名不对、任意位置改一个字符、前缀和编码不对、空令牌、签不出的授权；流密钥往返、加入步进（新人打不开以前的）、离开换代（离开的人打不开以后的）、改密文和改头（档、标志、层、计数器）都被拒、重放、乱序、计数器不重复、伪造的远步号不前移、旧一代保留 2 秒；转发器那一跳的消息往返和边界；对转发器：坏令牌、过期、角色不对、第二个发布者、别的房间各得到相应的拒绝，被拒的端随后被断开，会话协议的问候（另一个标记）握手不成功并计数，观看端发 `Subscribe` 被断开、分享端收到 `PeerLeft`，40 个握手后不说话的端（同时最多 32 个、5 秒后清空），满房间 `BUSY`，管道洪水（1 Mbit/s 的预算下 3 秒灌约 2 MB：约 0.2 MB 通过、约 1800 个数据报丢弃），没订阅的观看端收不到画面、订阅后从关键帧开始。
- 会话场景 `forwarder`：转发器改动经过的画面（每 5 张改一个字节），观看端拒绝并计数、画面照常；一个观看端离开后分享端换代、留下的人打开新一代。`forwarder_refusals`：过期令牌、角色不对、改过的令牌、别的房间、第二个分享端、转发器公钥对不上、没有分享端的房间，各得到写明原因的 `REFUSED`、`BUSY`、`AUTHENTICATION`、`NETWORK`，分享端不受影响。
- 公网入口、缓存和令牌：`rcsdk.websocket`（`tests/websocket.cpp`）：开头的每一种拒绝（太长、头太多、不是 GET、没升级、别的版本、坏的键、折行、没有 Host、目标不是路径）、客户端对回答的核对、帧的两个方向（拆开、逐字节喂）、RFC 6455 禁止的每一种帧和每一个上限、三种分帧。`rcsdk.forwarder` 加：令牌的受众（给别的转发器的被拒）、流密钥按关键帧推步和新人从留着的那一步开始（更早的打不开）、改过的声音和换了类型的头被拒；对真的 TCP、WebSocket 入口：426、405、400、431 的回答，未掩码、文本、1 MiB 的长度、保留位的帧各得到 1002、1003、1009、1002 并被关掉，TCP 上 0 字节的数据报断开，只发半个开头的连接 5 秒后关、不承载那一跳的连接 10 秒内关，13 条连接对 12 的上限立即关掉多的；分享端超出 `max_publish_bitrate_bps` 的画面被丢；多个房间互不串；缓存超过字节上限就不留。会话场景 `forwarder_entries`（TCP 分享端、WebSocket 和 UDP 观看端）、`forwarder_late`、`forwarder_uplink`、`forwarder_rooms`（第三个房间 `BUSY`）、`forwarder_token_refresh`（过期后没有新令牌的以 `REFUSED` 失败）；`rcsdk.forwarder_tls`：`wss://` 经 TLS 终结器（钉住的证书看得到画面，钉错的以 `AUTHENTICATION` 失败）。
- 宿主通道上的那一跳和浏览器观看端：会话场景 `forwarder_host`（那一跳走测试自己开的 WebSocket，掩码、每条消息一个数据报，和浏览器包同一条路；另一条走到 TCP 入口的 TCP 连接；关掉后问宿主要新令牌、经新的连接重连）；`rcsdk.c_api`（链路、通道、地址、`host_max_datagram` 的检查，能力位；wasm 里别的链路 `UNSUPPORTED`）；`rcsdk.websocket`（浏览器样子的开头：Chromium 的字段、UTF-8 的 Cookie、压缩扩展不回答）；`rcsdk.web_serve`（`serve.mjs` 的令牌只给自己的页面、只凭它的令牌或一次性的刷新票、不从 URL 拿，`rcsf1` 的签名和字段，`/forwarder` 只给自己的页面并原样转发）；`rcsdk.forwarder` 的 `behindAlone`。
- Linux（Ubuntu 24.04 arm64 虚拟机）：GCC 13.3 和 BearSSL 编的转发器，`rcsdk.forwarder`、`websocket`、`forwarder_tls` 和经它的 9 个会话场景全过；装成 systemd 服务后的进程状态见上表“转发器被攻破”一行。
- 模糊测试（FW-9）：`tests/fuzz/forwarder_entry.cpp`，有状态：一条连接的字节分多次读，四种读法（转发器的 TCP 入口、WebSocket 入口；端那一边对开头的回答和服务器的帧、TCP 的长度），步骤拼出原始字节、头和长度由输入给的帧（边界长度、非最短的长度写法、超过 2^63 的长度、掩码对错）、带长度的数据报，再按输入给的大小交出去；入口的读法就是入口自己的 `EntryReader`（`entry.cpp` 为此把读的那一段拆了出来）。每次读都核对：数据报 1..65535 字节、回给对端的不超过读进来的加一个回答、关了之后什么都不收。Homebrew LLVM 23 的 libFuzzer + AddressSanitizer + UndefinedBehaviorSanitizer，在 Mac（M4 Pro）上跑了约 0.55×10^6 + 5.03×10^6 次（30 秒 + 10 分钟，约 8400 次/秒，覆盖 875 条边后不再增长），零崩溃、零报告；改了字段值的 obs-text 之后在留下的语料上又跑了 5 分钟（2.72×10^6 次，约 9000 次/秒），同样零发现。共约 8.2×10^6 次、15.5 分钟。

**残余风险**：

- **FW-1〔中〕元数据**：转发器看得到房间名、路由号、谁在线、每张画面的大小、时间、档和关键帧标志（静止的屏幕上按键产生小画面，节奏可见）。指南建议房间名、路由号用不透明的值；流量形状暂不处理。
- **FW-2〔低〕换代窗口**：离开的人在新一代启用前（全部确认或 1 秒）仍能打开转发器若继续发给它的画面；诚实的转发器收到 `PeerLeft` 后立即不再发。
- **FW-3〔低〕令牌**：过期前可反复使用（重连需要）、没有非对称签名（签令牌的密钥转发器也有）；建议令牌短期。已有受众（共用一把密钥的几台转发器之间不能互用）和重连前的刷新回调（`refresh_token`，令牌可以更短）。
- **FW-4〔低〕占位**：持有效订阅令牌、但过不了端到端握手的端在房间里占一个名额直到它离开（过了握手却被拒绝的，分享端会让转发器断开它）。令牌是宿主签给自己用户的，宿主可以只签短期的、`rcsfwd_close_room` 清房间。
- **FW-5〔参考〕吞吐和 CPU**：每个端一个读线程加连接线程。本机环回和 2 核 ARM64 虚拟机上量过（[PERFORMANCE](PERFORMANCE.md) 的“转发器的吞吐”）：CPU 用满时延迟崩到秒级，过载的观看端每人最多攒 8 MB，`max_ends`、`max_viewers_per_room` 要按机器设（[指南 24](guide/24-forwarder.md#一台转发器带多少人)）；公网上没量；Windows 版只交叉编译过。
- **FW-6〔参考〕SA-09 的反射比**同样适用于转发器的端口（它用同一个 `UdpAcceptor`），随 SA-09 一并修。
- **FW-7〔低〕缓存的窗口**：为了让晚来的人立刻有画面，新人拿得到转发器留着的最近关键帧所在那一步的密钥，能打开它被放行之前至多 `cache_ms`（默认 3 秒）封装的画面和声音（转发器即使录下更早的密文，也给不了能打开的）。默认开着（晚来的人立刻有画面）；不想要的分享端把 `rcs_forwarder_config.from_admission` 设 1（新人只从下一个关键帧开始，会话场景 `forwarder_late` 测过）。
- **FW-8〔参考〕TLS 在代理**：`wss://` 的安全取决于宿主的代理配置（证书、协议版本）；代理到转发器那一段是明文的 WebSocket，里面仍是认证过的那一跳。指南 24 给了 nginx、Caddy、IIS 的示例，测试只用了 Python 的终结器。没有 HTTP 代理的 `CONNECT`。
- **FW-9〔参考〕入口的解析器**：已有有状态的模糊目标 `forwarder_entry`（上面“测试”），两端的 WebSocket 开头和帧、TCP 的长度分帧，ASan + UBSan 下零发现；它只在手动的模糊构建里（`RCSDK_BUILD_FUZZERS`），没进 CI。

### 3.10 TCP 端口（`src/protocol/tcp_demux.*`、`RCS_TRANSPORT_TCP`）

TCP 上跑的是和 UDP 同一个数据报连接，分享端的 TCP 端口是按连接编号分路的 `TcpDemux`，观看端的 TCP 断了、网络变了就再连一条接着走；TCP 上没有单独的记录通道，`SecureChannel` 只有助手链接用。由此带来的攻击面：

- **新的 TCP 连接可以接上已有的连接**：分路器只看明文的连接编号（32 位，只在路径上明文），不碰密钥；分享端只有在连接用自己的密钥认证了一条 TCP 连接上来的数据报、并且在它上面的挑战得到回答之后，才让它承载流量。别的密钥封的、重放的数据报不让连接动；看得到流量的攻击者扣下没送到的数据报、从自己的 TCP 送来，它是最新的，分享端去挑战那条 TCP（不超过从它收到的 3 倍字节，每秒最多开始 4 次），答不出，真正的观看端更新的流量让连接留在原处（`tests/tcp.cpp` 的 `movesNowhere`）。
- **资源**：还没归属任何连接的 TCP 连接最多 16 条（再来一条，最早的关掉，计入 `rejected_connections`），连上 2 秒不说话的、握手超时再过 2 秒还没归属的关掉；一个连接最多 4 条，按连接最近往上发过东西的先后挤（连接只往认证过的数据来的地方发，攻击者的 TCP 挤不掉在用的那条）；每条入一帧（≤ 64 KiB）加一次读、出 256 KiB；连接的队列 8 MiB；握手照旧 16 个并行、Retry cookie（`flood`、`handshakesSideBySide`、`descriptorShortage`）。
- **放大**：TCP 的三次握手已经证明了对端的地址，没有反射。
- **TCP-1〔参考〕能打断、不能接管**：看得到流量的攻击者知道连接编号，可以用假的 TCP 连接挤掉观看端备用的那条，或者复位、丢包，和 UDP 上一样能让路径停滞、换路径，不能接管，也读不到内容。
- **TCP-2〔参考〕半开的旧连接**：网络没了而 TCP 还不知道的旧连接在分享端留到连接结束或被新的挤掉（每个连接最多 4 条），各占一个描述符和最多 256 KiB 等着写的数据。
- **TCP-3〔参考〕有丢包的路径没实测**：有序路径上的计时是按原理定的，白发的重传和码率的反应要在实机上量。

测试：`tests/tcp.cpp`（上面几项、换一条 TCP 后输入恰好一次按序、迁移关着时 TCP 关了两端马上结束）和会话的 `migration_tcp`、`path_hold_tcp`、`path_hold_expired_tcp`、`tcp_no_migrate`、`standby_tcp`，都在 Mac 的回环上跑过。

## 5. 汇总

### 发现表

| ID | 严重度 | 标题 | 位置 | 验证 | 状态（第 0 节） |
| --- | --- | --- | --- | --- | --- |
| SA-01 | 高 | PIN(CPace) 观看端侧无计数，单连接可试数百次猜测 | `src/protocol/udp_connection.cpp:1868-1917`；`src/transport/handshake.cpp:237-270` | 测试验证（352 次/877ms 猜中） | 已修（第二轮） |
| SA-02 | 中(浏览器)/低 | web JSON 无限递归栈溢出 + 浮点→整型越界 UB | `web/json.hpp:146`、`web/json.hpp:45` | 模糊测试验证（ASan/UBSan 崩溃） | 已修 |
| SA-04 | 中 | 未认证 PIN 问候锁死分享端 PIN 认证(DoS) | `src/transport/handshake.cpp:298-304`；`credentials.cpp:36-43` | 测试验证 | 已修 |
| SA-05 | 中 | Unix agent 端点按路径 chmod + 目录归属不验 | `src/transport/local_stream.cpp:576-589,519-545` | 代码审阅 | 已修 |
| SA-06 | 中 | WINDOW/APP 限定不覆盖键盘(媒体键/全局快捷键) | `windows/input.cpp:191-205`；`macos/input.mm:211-221`；`include/rcsdk/rcsdk.h:862-869` | 代码审阅 | 已修(有残余) |
| SA-07 | 低 | macOS 助手按 PID 识别 + 链接密钥全可读 | `src/transport/local_stream.cpp:502-511` | 代码审阅 | 已修 |
| SA-08 | 低 | CAD 中转信任客户端 PID + 单实例 DoS | `components/supervisor/src/supervisor_windows.cpp:256,149,249` | 代码审阅 | 第二轮已修（Windows 上测过） |
| SA-09 | 低 | UDP ServerHello 约 2.34× 可反射 | `src/protocol/udp_connection.cpp:1783-1799` | 测试验证(比值 2.34×) | 已修（第二轮） |
| SA-10 | 低 | 端点名抢占致会话无法启动(DoS) | `src/transport/local_stream.cpp:321` | 代码审阅 | 已修报错 |
| SA-11 | 中 | 文件传输流控窗口可被重发 OFFER 重置→内存耗尽 | `modules/file_transfer/src/common.hpp:38`、`session_half.cpp:939,1386` | 代码审阅；第二轮测试验证 | 第二轮已修 |
| SA-12 | 低(中) | ICE 不过滤候选地址→向内网/元数据/组播发 STUN(类SSRF)+20×放大 | `src/transport/ice.cpp:210-230,596-617,623-644` | harness 验证 | 已修（第二轮） |
| SA-13 | 低 | Android BearSSL 信任用户添加的 CA | `src/transport/tls_bearssl.cpp:599-601` | 代码审阅 | 已修 |
| SA-14 | 低 | 浏览器永久保留每个指针形状→标签页内存耗尽 | `web/glue.cpp:337-343`、`web/rcsdk.mjs:182`、`src/session/cursor_stream.cpp:133-139` | 代码审阅 | 已修 |
| SA-15 | 低 | 密钥/PIN/口令留在 WASM 共享堆+指南缺 CSP 建议 | `web/rcsdk.mjs:115-133`、`web/glue.cpp:565-586` | 代码审阅 | 已修 |
| SA-16 | 低 | 文件传输向对端泄露并记录本机绝对路径 | `modules/file_transfer/src/session_half.cpp:1507,1528`、`machine_half.cpp:28` | 代码审阅 | 第二轮已修 |
| SA-17 | 低 | Windows 上 OVERWRITE 经 8.3 短名替换长名不同的文件 | `modules/file_transfer/src/files_windows.cpp`（`openIn`、`rename`） | 代码审阅；第三轮 Windows 测试（只交叉编译） | 第三轮已修（要在 Windows 上跑测试） |
| 第二轮新发现 | 中/低 | SA-11 的发送端一半、条目失败绕过窗口、发送端列目录的 TOCTOU、不说续传的清单动别人的续传文件、同时在收的传输不一起算空间 | 见第 0 节“第二轮” | 代码审阅 + 测试 | 第二轮已修 |
| 多项 | 参考 | ICE 预检大小(F7)、TURN 凭据/LIFETIME(F8)、证书吊销/CA(F9)、canvas 尺寸(F4)、示例开放中继+URL 带密钥(F12)、信令不保密(F13)、剪贴板同步(F14) | 见正文 | 审阅/harness | F14 已写进文档，F7、F8 已修（第二轮），F4、F12 已修、F9 部分修部分写明（第三轮），F13 不改 |
| SA-03 | 低/参考 | 已认证对端 inbound 队列无显式上限 | `src/protocol/udp_connection.cpp:615` | 代码审阅；第三轮测试验证 | 第三轮已修 |
| F7/F8/F9 | 参考 | CAD 无独立开关 / 看护者接受 UNC / secure_desktop 关时轮询竞态 | `share_agent.hpp:146-148`；`api.cpp:120`；`media_agent.cpp:258` | 代码审阅 | 第三轮：F7 写明，F8 已修，F9 已修（只交叉编译） |
| 第三轮新发现 | 低 | 剪贴板文件名比文件传输的规则松、预取按复制说的而不是列出的、macOS 无限制地把对端 PNG 转 TIFF；BearSSL / Windows 接受不是 TLS 服务器的证书 | 见第 0 节“第三轮” | 代码审阅 + 测试 | 第三轮已修 |
| TCP-1..3 | 参考 | TCP 端口：假的 TCP 连接能打断不能接管、半开的旧连接、有丢包的路径没实测 | `src/protocol/tcp_demux.*`、`src/protocol/udp_connection.cpp` | 测试（`tests/tcp.cpp`）+ 代码审阅 | 写明（3.10） |
| FW-1..9 | 中/低/参考 | 转发器：元数据、换代窗口、令牌、占位、吞吐、反射比、缓存窗口、代理上的 TLS、入口解析器的模糊测试（已做，零发现） | `server/forwarder/`、`src/session/forwarder.*`、`src/protocol/media_keys.*`、`src/transport/websocket.*` | 测试（`rcsdk.forwarder`、`rcsdk.websocket`、`rcsdk.forwarder_tls`、会话场景 `forwarder*`）+ 审阅 | 写明；FW-3 已做受众和刷新 |

### 已验证的发现
用 `tests/security.cpp`（环回、全部通过）验证了 **SA-01、SA-04、SA-09**；用 ASan+UBSan+libFuzzer（约 1.06×10^7 次执行）验证了 **SA-02**，并佐证其余二进制网络面解析器零崩溃；网络部分的专项审阅另用 ASan/UBSan harness（2M STUN、500k TURN 数据报、300k `rcs1.` 信令）验证了 **SA-12** 的地址可达性且 STUN/TURN/ICE 解析零内存/UB 错误。

### 已检查且稳健
AEAD nonce/密钥与序号唯一、常量时间比较、ECDH 非法点拒绝、CPace 低阶份额拒绝、降级/秘密类型混淆防护、助手链接记录的严格序号与数据报的 1024 重放窗、64 MiB 记录上限、全部二进制 wire 解析器边界+值域校验（模糊测试佐证）、STUN/ICE/TURN/指针 RLE 边界、握手洪水 cookie/并行推进、PBKDF2 强制重算防护、控制授权 epoch 门与按键释放、输入双重校验、Windows 管道 DACL 与看护者启动卫生、SAS 管道、链接握手顺序与密钥绑定；**TLS to TURN 三后端均默认校验证书与主机名**（配置拒绝无 pin 且无 DNS 名字的 IP `turn_server`；Apple verify block 仍评估 `SecPolicyCreateSSL(true,name)`；Windows 手动校验要求 serverAuth EKU + 服务器名、重协商后复查；BearSSL SNI+名字校验、pin 模式、无锚失败关闭），无覆盖校验的旁路（见第 3.8 节）；浏览器端到端用 SDK 自己的握手/AEAD 不依赖 WebSocket TLS、要求跨源隔离、无 window message 监听、对端文件名严格 UTF-8+组件校验（第 3.5 节）；**日志默认关**，grep 全树未见密钥/PIN/口令/ICE 凭据/TURN 凭据/信令文本（仅 DEBUG 记其长度，`session.cpp:1489`）/剪贴板/窗口标题入日志。

### 残余风险与建议（优先级从高到低）
第一轮修复后（第 0 节）：第 6 条的浏览器加固除 F4 外已做，第 7 条里 SA-06、SA-13 已改；其余各条仍待后续。第二轮部分 a 后：第 2 条已逐行审阅并修好（剩 SA-17 和写明的“共享接收目录”）；第 3 条的隐藏 / 瞬态标记已审，解码、方向规则、无用户动作注入仍待审；第 7 条的 SA-16 已改。第二轮部分 b 修了第 1 条、第 4 条（速率限制除外）和第 7 条的 TURN LIFETIME。第三轮（第 0 节）：第 2 条剩下的 SA-17 已修（只交叉编译）；第 3 条已审完并修了三处；第 4 条的速率限制归宿主（写明）；第 5 条的有状态模糊目标已加并跑过（纳入 CI 还没做）；第 6 条的 F4 和第 7 条的 F12 已修。
1. ~~修复 **SA-01**（观看端限制每问候一个应答并计数）与 **SA-04**（仅对已证明地址的 PIN 握手计尝试）——PIN 功能的核心安全目标。~~ 第二轮已修。
2. **原生文件传输语义安全**（符号链接/junction 的 TOCTOU、覆盖规则、续传令牌、磁盘耗尽、创建权限）——本轮未逐行确认；建议专门审阅 + 定向测试，并写有状态模糊目标。模块的专项审阅已确认 SA-11（流控窗口重置）；其余条目在本轮未能全部汇总进来，续审从此处接续。
3. **剪贴板**隐藏/瞬态数据标记、图像/HTML/RTF/文件列表解码与大小限制、方向规则、无用户动作注入——专门审阅。
4. **SA-12 ICE 候选过滤**：加通用地址类别掩码/过滤回调，默认丢弃内网/元数据/组播/广播；对信令加速率限制与大小预检（F7）。
5. **模糊测试硬化**：把本次 `fuzz_parsers` 与网络 harness 纳入 CI；补充**有状态**模糊目标：UDP 分片+FEC 重组（`protocol/udp_connection.cpp` 的 `receiveFragment/receiveParity/recover`、`protocol/fec.cpp`）、文件传输 store/续传状态机、安全通道记录流。
6. **浏览器加固**：`web/json.hpp` 加递归深度上限（约 64）与 double→int 饱和、修正 `\u` 转义与代理项处理（F11）；指针形状 Map 设 LRU（SA-14）；`_free` 前清零秘密并把 CSP/禁第三方脚本写进指南（SA-15）；缩放前核对解码尺寸（F4）。
7. **文档修正**：SA-06（WINDOW/APPLICATION 限定不应作为安全边界宣传，落到 rcsdk.h / guide 07）；SA-16（只把相对路径发给对端）；SA-13（Android 用户 CA 默认排除或加开关）；TURN LIFETIME 钳制（F8）；示例 `examples/web/serve.mjs` 的开放中继与 URL 带密钥（F12，测试软件但宿主可能照抄）。

---
*测试工件*：`tests/security.cpp`（第二轮起是 SA-01/04/09 的回归测试）。第一轮的模糊 harness、崩溃样本与仪器化构建未入库；第三轮的有状态模糊目标入库在 `tests/fuzz/`（`RCSDK_BUILD_FUZZERS`）。
