# 写一个模块

SDK 分成**核心**和**模块**。核心把画面、声音、输入安全地从一端送到另一端；模块往会话里加一项功能。官方模块（剪贴板 `rcsdk.clipboard`、缩略图 `rcsdk.thumbnail`、文本插入点 `rcsdk.text-caret`、电源 `rcsdk.power`、文件传输 `rcsdk.file-transfer`）和别人写的模块用的是同一套接口 `rcsdk/module.h`；官方模块不走任何后门，只包含公开头文件，CMake 用 include 路径保证这一点。

本页讲怎样**写**一个模块，以及官方模块怎样用这套接口。宿主怎样**用**模块（注册、启用、设置、事件、各个官方模块的函数）见开发者指南的[模块](guide/15-modules.md)。接口的正式说明在 `include/rcsdk/module.h` 的注释里。

## 1. 为什么这样分

- **核心小而完整**：不装任何模块也是完整的远程桌面 / 屏幕共享 SDK。剪贴板、文件传输、消息这类边缘功能不和会话、协议、助手链接缠在一起（文件传输一开始就是模块，核心没有为它改一行）。
- **按需启用**：模块注册了、会话里给了它的设置才启用；没启用的模块没有线程、没有消息、没有调用。
- **第三方和官方平起平坐**：握手时两端互报模块、按观看端收发、在被控那台机器上跑一半（助手里）、有事件有统计，这些官方模块用的能力第三方模块全都有。官方模块需要而接口没有的，就补进接口，所有人一起得到。

## 2. 宿主那边发生什么

写模块的人要知道宿主怎样把模块接进会话（详见[指南 15](guide/15-modules.md#用一个模块)）：

- 宿主在进程里注册一次（`rcs_register_module`，在用它的会话和助手创建之前），再在会话的扩展链里给它设置（`RCS_EXT_MODULE`、`rcs_module_config`）才启用。SDK 复制设置的 `size` 字节（最多 64 KiB），先交给模块的 `check` 验证：不合法是 `INVALID_ARGUMENT`、本系统做不到是 `UNSUPPORTED`，原因由模块写。设置里指向的东西（回调的 `user_data`）仍归宿主。
- 运行中 `rcs_session_update` 带上同一个模块的设置：先 `check`，再交给 `update`。会话用哪些模块在创建时就定了。
- 宿主看到的是一种事件 `RCS_EVENT_MODULE`：核心自己发 `RCS_MODULE_ACTIVE`、`RCS_MODULE_INACTIVE`（对端没用它，或版本不同）、`RCS_MODULE_NO_MACHINE`（另一个进程里的助手跑不了机器那一半）；`kind` 从 `RCS_MODULE_EVENT_OWN`（256）起是模块自己的，payload 的结构由模块的头文件定义。
- 统计：`rcs_session_get_module_stats` 给出每个模块收发的消息数和字节；模块自己的计数由它自己的函数给。
- 模块自己的函数是普通 C 函数，放在模块的头文件里；失败时用 `rcs_set_last_call_error` 设好 `rcs_get_last_call_error` 的原因，和别的建在 SDK 上的库一样。
- 分享端的助手在另一个进程（`rcs_agent`）时，那个进程也要注册同一个模块，机器那一半才在那里跑。

## 3. 写一个模块

一个模块就是一张函数表（`rcs_module_definition`）加一个 id。id 是 1～63 个字母、数字、点和连字符；`rcsdk.` 开头的留给官方模块，别人用自己的域名倒写（`com.example.whiteboard`）。注册时 SDK 复制这张表和 id。

| 字段 | 说明 |
| --- | --- |
| `version` | 模块自己的线上格式的版本。两端不同时这个连接上不激活；助手进程里的版本不同时机器那一半不跑 |
| `flags` | `RCS_MODULE_PEER`：和对端的同类说话（只在两端都用、版本相同的连接上激活）；`RCS_MODULE_MACHINE`：有机器那一半 |
| `roles` | 参与哪种会话，`1u << rcs_role` 位；别的角色的会话用它是 `INVALID_ARGUMENT` |
| `max_message_bytes` | 一条消息最多多大（1 字节～1 GiB），收发都按它；对端发来更大的就是违反协议，连接失败 |
| `user_data` | 每个回调都带回来 |

### 3.1 会话那一半

每个会话、每个模块一个句柄（`rcs_module`），从 `rcs_session_create` 活到 `rcs_session_destroy`。所有回调都可以是 NULL。

| 回调 | 什么时候 |
| --- | --- |
| `check(role, settings, size, reason, capacity)` | 创建会话和 `rcs_session_update` 时，验证设置；还没有句柄 |
| `start(module, role, settings, size)` | 每次运行开始（`rcs_session_start`）；设置是借给它的副本 |
| `update(module, settings, size)` | 运行中设置变了 |
| `connected(module, peer)` / `disconnected(module, peer)` | 会话的一个连接通过了握手 / 结束了：分享端是观看端的编号，观看端那边的分享端是 0。不管模块在那个连接上激活没有，每个连接都来：`connected` 在它的 `peer_joined` 之前，`disconnected` 在 `peer_left` 之后。给要知道“有没有人连着”的模块用，例如保持唤醒 |
| `peer_joined(module, peer, hello, size)` | 在 `peer` 那个连接上激活了（分享端是观看端的编号，观看端那边的分享端是 0），带着对方的问候 |
| `peer_hello(module, peer, hello, size)` | 对方换了问候 |
| `peer_control(module, peer, granted)` | `peer` 现在有没有远程控制：分享端是那个观看端的，观看端是自己的（`peer` 0，分享端授予的，同 `RCS_EVENT_CONTROL` 的 `granted`）。激活时正有控制就紧跟着 `peer_joined` 来一次，之后每次变化来一次；`peer_left` 之后不再来 |
| `peer_left(module, peer)` | 那个连接结束了；之后不会再有它的任何回调 |
| `received(module, peer, data, size, flags)` | 对端同类发来一整条消息；`flags` 有 `RCS_MODULE_UNRELIABLE` 表示是数据报 |
| `sent(module, peer, tag)` | 一条可靠消息连接全部接收了（发送时给的 `tag`） |
| `room(module, peer)` | 发往 `peer` 的队列在拒绝过一条之后又有空间了 |
| `machine_joined(module)` / `machine_left(module)` | 机器那一半开始运行 / 不在了（另一个进程里的助手离开了，或者被新的助手替换：新的跑起来时再来一次 `machine_joined`） |
| `from_machine(module, data, size)` | 机器那一半发来的消息 |
| `stop(module)` | 运行结束；这之前每个对端都有过 `peer_left` 和 `disconnected`，机器那一半有过 `machine_left`。下次 `start` 之前不会再有回调 |

### 3.2 机器那一半（`RCS_MODULE_MACHINE`）

跑在“机器所在的地方”：分享端在会话的助手里（默认在本进程，无人值守时在另一个进程，那个进程也注册了这个模块），观看端在它自己的进程里。它和会话那一半之间**只通过消息**联系（`rcs_module_send_machine`），在哪个进程里都一样，所以写一次两种情况都对。每一半有自己的句柄和自己的状态。

| 回调 | 什么时候 |
| --- | --- |
| `machine_start(module, role)` | 开始：`role` 是这台机器所属会话的角色 |
| `from_session(module, data, size)` | 会话那一半发来的消息，按发送的顺序 |
| `machine_stop(module)` | 结束：会话那一半停了，或者助手要离开。在这里结束模块自己用到这个句柄的线程，返回后句柄就没了 |

### 3.3 SDK 给模块的服务

| 函数 | 说明 |
| --- | --- |
| `rcs_module_send(module, peer, data, size, flags, tag)` | 发给对端的同类（`peer` 0：所有激活的对端，观看端只能用 0）。走会话的加密连接，可靠消息完整、按序到达，和数据通道一样排在输入之后、画面旁边；`RCS_MODULE_UNRELIABLE` 是数据报（最多 `RCS_DATA_MAX_DATAGRAM` 字节，UDP 上不重发）；`RCS_MODULE_REPLACE` 替换这个模块发往同一个对端、还没开始发的最新一条同样带 `REPLACE` 的消息（被替换的不会有 `sent`）。每个对端的队列最多装 `max_message_bytes` 字节：放不下是 `QUEUE_FULL`，之后有 `room`；`peer` 0 时要么全部排上、要么一个都不排。没有激活的对端是 `INVALID_STATE` |
| `rcs_module_set_hello(module, hello, size)` | 本端的问候（最多 1 KiB）：连上时随模块清单发出，运行中再设就发给已经交换过清单的对端，排在等着的消息前面 |
| `rcs_module_send_machine(module, data, size)` | 发给另一半（会话那一半发给机器那一半，或者反过来），按顺序；另一半没在运行是 `INVALID_STATE` |
| `rcs_module_emit(module, peer, kind, payload, size)` | 给宿主的事件，`kind` 从 `RCS_MODULE_EVENT_OWN` 起，payload 最多 64 KiB，复制。只有会话那一半能发 |
| `rcs_module_session(module)` | 它所在的会话，用来调宿主也能调的公开函数（`rcs_session_get_state`、`rcs_session_list_viewers`……）；机器那一半是 NULL |
| `rcs_module_find(session, id, &module)` | 模块自己的公开函数用它找到这个会话里的那一半；会话没用这个模块是 `INVALID_STATE` |
| `rcs_module_run(module, task, argument)` | 把模块自己的工作（它的公开函数）当作它的一次调用来做：排在正在进行的那次之后（在调用里面调用就是嵌套），返回 task 的结果 |
| `rcs_module_set_state` / `rcs_module_state` | 这一半的状态，和替换或句柄消失时调用的释放函数 |
| `rcs_module_log(module, level, message)` | 进日志，归属于它所在的会话或助手，模块名是它的 id |
| `rcs_module_get_shared(module, &shared)` | 机器那一半（分享端的助手里）：会话现在共享的是什么、在这台机器的屏幕上在哪里。`rcs_module_shared`：来源和显示器（0 解析成主显示器）、窗口、它的窗口或应用所属的进程，四条边用系统自己的屏幕坐标（Windows 是虚拟屏幕的物理像素，macOS 是全局点），右边和下边在外面一格。远程输入按同一块区域映射。会话还没在那里设好采集、窗口最小化或没了、区域不在显示器上时是 `INVALID_STATE`；会话那一半和观看端的机器上也是 `INVALID_STATE` |
| `rcs_capture_picture(what, max_width, max_height, &picture)` | 这台机器上，一次采集现在会共享的画面的一张图：检查和结果与会话的采集相同，宿主不开会话也能用（让用户选共享什么）。缩略图模块在会话的助手那里调它 |

模块的公开函数失败时用 `rcs_set_last_call_error(result, reason)`（`rcsdk.h`）设好 `rcs_get_last_call_error` 的原因，成功时用 `RCS_OK` 清掉。

### 3.4 规则

- **调用不重叠**：同一半的回调一个接一个，SDK 替它排队，所以模块状态不用为 SDK 的回调加锁；模块自己的公开函数用 `rcs_module_run` 进入同一个队列，也就不用锁。回调来自 SDK 的各个线程（读连接的、助手的、调 `rcs_session_update` 的宿主线程），要尽快返回，耗时的活放到模块自己的线程。服务函数线程安全，可以从模块自己的线程调。
- **事件在调用返回后才交给宿主**：在回调里 `rcs_module_emit` 的事件，等这次（最外层的）调用返回才送进 `on_event`。宿主在 `on_event` 里调模块的函数（要等这个模块）时，就不会和一个正在等 `on_event` 的模块互相等。模块在回调里直接调宿主自己的回调（剪贴板的 `on_clipboard`）不受影响。
- **一个模块调另一个模块的函数**（剪贴板调文件传输的 `rcs_file_*`）：被调的模块的调用嵌在调用者的调用里，它的事件等这个线程上**所有**模块的调用都返回才交给宿主（只等它自己的话，宿主在 `on_event` 里调外层模块的函数时会互相等）。只许一个方向：被调的模块不能在自己的调用里回调调用者，它要告诉调用者的事经回调在自己的线程上、不在调用里交出去（文件传输的 `on_fetch`、`on_read`……），调用者再用 `rcs_module_run` 进自己的队列。
- **生命周期**：会话每次运行 `start` 到 `stop`；连接来去是 `peer_joined` / `peer_left`；机器那一半随助手来去是 `machine_joined` / `machine_left`。会话那一半的句柄和状态活到会话销毁，所以上一次运行的计数在运行结束后还能读。
- **出错**：设置不对由 `check` 拒绝；模块在运行中出了问题只报告事件，不能让会话失败。对端违反 SDK 的协议（消息比 `max_message_bytes` 大、模块编号不在它的清单里）由 SDK 判定，连接失败，和其他协议错误一样。模块自己的消息格式由模块验证，对端可能是任何人写的。
- **版本**：模块接口和 SDK 同一个版本、同样精确检查 `struct_size`；模块自己的线上格式靠 `version` 两端比较。
- **安全**：模块在宿主进程里运行，写坏了能把进程带崩，这是宿主选择加载的代码。模块碰不到 SDK 的内部对象，只有上面这些 C 函数；身份和握手（口令、设备密钥、短 PIN）不对模块开放。模块的消息走同一条加密连接，模块之间看不到彼此的消息。

## 4. 线上和链接上怎么走

**连接上的模块清单**：每个连接上，观看端在交换编码格式提议之后先发自己的模块清单（每个 `RCS_MODULE_PEER` 模块的 id、版本、问候），分享端收到后回自己的。两端都在发出任何模块消息之前已经有了对方的清单：观看端的消息在它收到分享端的清单之后才发，分享端是在收到观看端的清单、回了自己的之后才激活。清单、问候和模块消息都是数据流上的 `DataPart`（新的 `kind` 字段区分：宿主的数据、模块消息、模块清单、模块问候），和宿主的数据消息同一条有序的数据流，大小不受一个控制消息的限制；模块编号是它在发送方清单里的位置（从 1 起）。数据报是 `DataDatagram`，多了模块编号；它可能比清单先到，这时模块还没激活，丢掉（本来就不保证到达）。

**发送队列**：每个连接的数据队列分成宿主的和每个模块各一个，轮流发、一次一条，大的模块消息不会让宿主的数据或别的模块等太久；清单和问候排在所有消息前面，新的问候替换还没发的旧问候。可靠消息照旧切成分段、限制在路上的字节数（`rcs_network_tuning_config.in_flight_bytes`），排在输入之后。

**助手链接**：`ModuleStart{id, version}`、`ModuleStop{id}`（会话 → 助手）、`ModuleStatus{id, result, message}`（助手 → 会话：跑起来了，或者为什么不能：没注册、版本不同、没有机器那一半），`ModuleData{id, index, count, bytes}`（两个方向，4 MiB 一段，同一个模块的一条消息的分段连着发）。新的助手接上时，会话要过的模块再要一遍，和画面、声音、输入的设置一样。

## 5. 官方模块怎样用这套接口

### 5.1 剪贴板 `rcsdk.clipboard`

`modules/clipboard/`，库 `RemoteControl::Clipboard`，头文件 `rcsdk/clipboard.h`。

- **会话那一半**（`src/session_half.cpp`，`Hub`）：本端最新的一份，按每个对端在问候里说的格式和大小裁好、问过宿主，用 `RCS_MODULE_REPLACE` 发（新的覆盖没发出的），`sent` 时报告“发出了”；收到的检查、交给宿主，再让机器那一半放进系统剪贴板，放好后分享端接着发给其他观看端。有对端激活时让机器那一半看剪贴板，最后一个对端离开时停。问候是 8 个字节：本端收的格式和最大字节数（不收是 0 和 0）。
- **机器那一半**（`src/machine_half.cpp`）：看剪贴板的线程（`Watcher`）和每个系统的剪贴板代码（Windows、macOS、iOS 的 UIPasteboard、Android 经宿主交出的 Java VM），在分享端的助手里或观看端的进程里跑；会话那一半告诉它“怎么看”“放这一份”，它回“新的一份”“读不了”“放好了 / 没放成”。消息格式在 `src/messages.hpp`，两个方向都检查（另一个进程里的助手也可能被攻破）。
- **文件**（`RCS_CLIPBOARD_FILES`）：剪贴板模块是系统剪贴板唯一的主人，文件的内容走文件传输模块的公开函数（`rcs_file_publish`、`rcs_file_fetch`、`rcs_file_read`），不碰它的内部。复制的一端给每个收文件的对端各发布一次，新的复制替换它时撤销，并用剪贴板自己的消息（`WITHDRAWN`）告诉对端，和复制走同一个有序队列。系统的粘贴要文件时，机器那一半（`src/pastes.cpp`）请会话那一半取（`READ` 或 `INTO` 一个目录），会话那一半调文件传输，结果再转回去；文件传输的回调在它自己的线程上来，经一个 `Link`（`rcs_module_run`）进剪贴板的调用顺序，剪贴板的会话那一半销毁时等正在进行的回调结束。这一半的问候里有没有 FILES，由机器那一半报告的“这个进程的剪贴板能不能做”决定（分享端的助手在另一个进程时就是那个进程）。
- 只包含 `rcsdk/clipboard.h`（它包含 `rcsdk/file_transfer.h`、`rcsdk/module.h`、`rcsdk/rcsdk.h`）；Windows 那几个小工具（COM 指针、宽字符转换）是模块自己的。库 `RemoteControl::Clipboard` 链接 `RemoteControl::FileTransfer`。

### 5.2 缩略图 `rcsdk.thumbnail`

`modules/thumbnail/`，库 `RemoteControl::Thumbnail`，头文件 `rcsdk/thumbnail.h`：在分享会话采集的那台机器上（助手在另一个进程、另一个账户、另一个 Windows 会话时就在那里）拍一张一次采集会共享的画面。

拍图本身留在核心里：Windows 一个进程同一时刻只能复制一个显示器一次，正在运行的采集要替缩略图拍，这是采集的一部分。核心把它作为服务给出（`rcs_capture_picture`，在 `rcsdk/module.h`），模块只负责“到会话的助手那里去拍”：

- **会话那一半**：记着在等的请求，`machine_left` 和 `stop` 时让它们以 `INVALID_STATE` 结束（等的人马上知道，不等到超时），五秒没回答是 `TIMEOUT`。
- **机器那一半**：在自己的线程上一张一张拍（一次采集可能等系统好几秒，助手的其他模块照常），用 `rcs_capture_picture`，所以检查的是助手那台机器、那个进程。
- 只参与分享会话（`roles` 只有 SHARE），不和观看端说话（没有 `RCS_MODULE_PEER`）。

### 5.3 文本插入点 `rcsdk.text-caret`

`rcsdk.text-caret`（`modules/text_caret/`，库 `RemoteControl::TextCaret`，头文件 `rcsdk/text_caret.h`）：被控机器上现在打字会落到哪里（有键盘焦点的文本框的插入点），让观看端的输入法在那里显示组字和候选。两端都用这个模块：观看端有控制时要，分享端在有观看端要、并且有控制时，在它的机器上看，变了才告诉它。

它用到的两样接口是为它加的，但都是通用的：

- **`peer_control`**：分享端只在观看端有控制时看，观看端只在自己有控制时报告；控制收回时，已经显示的插入点报告为没有了。
- **`rcs_module_get_shared`**：机器那一半看到的插入点是屏幕坐标，要换算成“共享的那块区域里的输入坐标”（0～65535，和远程输入一样），它问核心现在共享的是哪块区域、属于哪个进程（共享一个窗口或应用时，焦点在别的应用里就没有插入点）。

Windows（系统插入点，没有就用 UI Automation）和 macOS（辅助功能）读插入点的代码在模块里（`src/reader_windows.cpp`、`src/reader_macos.mm`），它们需要的小工具（COM 指针、DPI 范围、CF 引用）是模块自己的。宿主自己执行输入（`RCS_INPUT_HOST`）时，模块看的仍是系统的插入点，和宿主执行的输入落到哪里可能无关，这种宿主不用这个模块就行。

### 5.4 电源 `rcsdk.power`

`modules/power/`，库 `RemoteControl::Power`，头文件 `rcsdk/power.h`：会话期间机器保持唤醒（分享端让观看端连得上、看得到；观看端让自己的屏幕在看的时候不熄）、网络唤醒的积木、观看端请分享端的机器重启 / 关机 / 睡眠 / 注销。怎么用见[指南 15](guide/15-modules.md#电源rcsdkpower)。

- **两半**：会话那一半按设置和连接（`connected` / `disconnected`：每个连接都来，对端用不用这个模块都算）决定要什么，告诉机器那一半；机器那一半在助手里（本进程或另一个进程，那个进程也要注册这个模块）或观看端自己的进程里向系统要电源请求，原因是“Remote session in progress”：Windows 的 `PowerCreateRequest`（`powercfg /requests` 能看到）、macOS 的 `IOPMAssertion`（`pmset -g assertions` 能看到）、iOS 关掉应用的空闲计时器（只有 `DISPLAY`，`SYSTEM` 是 `UNSUPPORTED`）、Android 经宿主交出的 Java VM 拿唤醒锁。
- **和对端说话**（`RCS_MODULE_PEER`，`src/messages.hpp`）：分享端的问候是它提供的操作（宿主允许的 ∩ 机器那一半能做的）、告不告诉唤醒信息、最多 8 个网卡；观看端的请求 `Ask` 和分享端的答复 `Answer`。保持唤醒不看模块激活没有，只看连接。
- **机器那一半的另外几件事**：起来时告诉会话那一半它能执行哪些操作（`Can`，`rcs_power_status.actions`）；在自己的线程上列出能唤醒机器的网卡（`List` / `Listed`，Windows 的设备列表可能要上百毫秒）；收到 `Act` 时检查系统、权限和标志，答复 `ACCEPTED` 或 `REFUSED`，接受的在一个不随会话结束的线程上等延迟再执行，结果（`STARTED` / `FAILED`）在机器那一半还在时报回去。进程里同一时间只有一个。
- **系统的那一半在接缝后面**：`src/system.hpp` 的 `Machine`（能做什么、检查、执行、列网卡），每个系统一份（`system_windows.cpp`、`system_macos.mm`、`system_none.cpp`）；测试用 `setMachine` 换成替身，**从不真的执行**。魔术包（`src/system.cpp`）POSIX 和 Winsock 同一份，浏览器里 `UNSUPPORTED`。
- **报告**：`RCS_POWER_CHANGED`（`rcs_power_status`）、`RCS_POWER_ACTION`（每个请求的每一步，两端）、`RCS_POWER_PEER`（观看端：分享端提供和告诉的变了）；系统拒绝、这个系统做不到、机器那一半没在跑都写明原因，运行照旧。

### 5.5 文件传输 `rcsdk.file-transfer`

`modules/file_transfer/`，库 `RemoteControl::FileTransfer`，头文件 `rcsdk/file_transfer.h`：文件和目录从一端到另一端，两个方向都行。线上格式和安全规则见 [FILE-TRANSFER-DESIGN.md](FILE-TRANSFER-DESIGN.md)，怎么用见[指南 15](guide/15-modules.md#文件传输rcsdkfile-transfer)。

- **会话那一半**（`src/session_half.cpp`，`Hub`）：两端各自的传输表；清单、接受、块、确认、条目结果、取消、暂停都是模块消息（`src/messages.hpp`）。问候 29 字节：本端允许的方向、本次运行的随机编号（重连回来的同一端靠它认出来，分享端那里观看端编号会变）、条目数和字节的上限。每次传输最多 4 MiB 没被对方确认写好：读之前算进去，对方每写好一块回一个确认才减掉；交给自己存储的读、写在存储回答之前一直算，对端重发清单或回答不让它从头算（见 [FILE-TRANSFER-DESIGN](FILE-TRANSFER-DESIGN.md) 3.4）；每个对端的模块队列满了（`QUEUE_FULL`）就排在自己的等待队列里，`room` 时按顺序发。连接结束（`peer_left`）时传输等待，同一端回来（`peer_joined`，运行编号相同）时发送方重发清单，接收方回“从哪里继续”和新的纪元，旧纪元的块丢掉。
- **机器那一半**（`src/machine_half.cpp`、`src/store.cpp`）：在分享端的助手里或观看端的进程里，自己的一个线程一件一件地做：列目录（不跟随符号链接）、读块、写临时文件、改名（四种冲突规则）、设时间、查剩余空间、读写续传记录、丢弃。和会话那一半之间只有消息，两个方向都检查。助手中途离开（`machine_left`）时传输等待，新的助手接上（`machine_joined`）后接收方回“从已写好的地方继续”，发送方把没读回来的块重新问一遍。传输结束时，会话那一半把结束要做的事（设目录的时间、删记录或临时文件、关文件）作为这次传输的最后一个请求交给它，它按顺序做完后回答，会话那一半这时才发 `RCS_FILE_DONE`：两半之间只有消息、对方在另一个线程或进程里，“交出请求”不等于“做完”，要等回答。它在回答之前离开就不等了，日志写明没做完的。
- **文件系统**（`src/files_posix.cpp`、`src/files_windows.cpp`）：模块自己的，不用 `std::filesystem`：POSIX 的 `O_NOFOLLOW`、`renamex_np(RENAME_EXCL)`、`futimens`；Windows 的 `\\?\` 长路径、名字代理类重解析点（符号链接、目录联接）当链接、`MoveFileExW`、`SetFileTime`、`GetDiskFreeSpaceExW`。剩余空间有测试替身（`files::setFreeSpace`）。
- **`target = HOST`**：会话那一半自己的一个线程调宿主的 `read`、`write`、`resume`，结果用 `rcs_module_run` 交回模块的调用顺序里，所以和机器那一半走同一套逻辑（传输结束时也一样等这个线程做完之前排着的调用）。
- **发布和取**：发布只在会话那一半记下条目和对端（同一端在 `reconnect_wait_ms` 内回来仍可取），对端的 `FETCH` 到了才变成一次发送的传输（每次取都在机器那一半重新列目录）；`READ` 的取由取的一方要一段、源头经机器那一半读出来回一段，按要的顺序。给发布和取的回调（剪贴板模块就靠它们）由会话那一半自己的一个线程（`Notifier`）一个一个调，不在模块的调用里，所以回调里可以调模块的函数，也可以用 `rcs_module_run` 进别的模块的队列。
- **时钟**：清单超时、等对端回来、限速、每 250 ms 最多一次的进度，由会话那一半自己的线程每 50 ms 用 `rcs_module_run` 推一下（只在有传输时）。
- **宿主在 `on_event` 里调它的函数**（例如收到清单就接受）：做这个模块时发现，会话调用 `on_event` 时持有事件锁，宿主函数引起的事件在同一线程上再拿这把锁就死锁（剪贴板的 `rcs_clipboard_send` 也一样）。核心因此这样处理：`on_event` 里的调用引起的事件先存下，等这次 `on_event` 返回后在同一线程上依次送出，`on_event` 仍然不重叠；模块自己不用做什么。

## 6. 第三方模块的例子

`examples/pointer/`：`com.example.pointer`，观看端告诉分享端它指着屏幕的哪里（数据报，最新的为准），分享端宿主收到 `EXAMPLE_POINTER_MOVED` 事件后可以在那里画个标记。不到 100 行，只用公开头文件。`rcsdk_cli --pointer`（两端都加）：观看端绕着屏幕中间画圈，分享端每 20 次打印一次位置。

## 7. 不做的

- 不从磁盘加载插件（`dlopen` 一个目录）。模块就是宿主链接进来的库，由宿主注册；要插件目录的宿主可以自己加载 DLL 再注册，SDK 不替它决定从哪里加载、信任谁。
- 模块不进每帧的视频路径（采集、编码、发送、解码），以免拖慢画面；那条路径的扩展还是宿主编解码器、宿主传输这些接口。
- 不做替换流水线某一环的“提供者”和多方依次表态的“拦截链”（先只做功能模块）。
- 模块不能让会话失败、不能改核心的设置、不能看别的模块的消息、不能参与身份和握手。

## 8. 以后

- 参数的自我描述（模块列出参数的名字、类型、范围、默认值，让不认识这个模块的宿主也能生成设置界面）：先不做。
- 助手（`rcs_agent`）有自己的头文件 `rcsdk/agent.h`，拆成自己的库：先不做（看护者和会话列表已经是单独的库 `RemoteControl::Supervisor`）。
- 预设不管模块的设置（见 [API-DESIGN](API-DESIGN.md#6-预设的取值)）。
- `rcsdk.` 前缀只是约定，注册时不检查。

每个官方模块验证到哪一步，见 [ROADMAP 的“当前状态”](ROADMAP.md#当前状态)。
