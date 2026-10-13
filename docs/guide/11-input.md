# 11 远程控制与输入

## 分享端：允许控制，`rcs_control_config`

| 字段 | 内容 | 默认 |
| --- | --- | --- |
| `allowed` | 1 允许观看端控制。一对多时是新来者的默认，运行中改会设置所有在场的观看端（单个观看端用 `rcs_session_set_viewer_control`，加入时用 `on_viewer_join` 的 `decision->control`） | 0 |
| `target` | `RCS_INPUT_SYSTEM`：本机执行（注入到系统，需要输入权限）；`RCS_INPUT_HOST`：全部交给宿主的 `on_input`，不碰系统、不需要权限 | `SYSTEM` |
| `host_input_types` | `target` 为 `SYSTEM` 时交给宿主执行的类型（`1u << rcs_input_type` 位），例如只接管触摸或笔给自己的画布，键鼠仍由系统执行；需要 `on_input` | 0 |

1. 分享端允许控制（C++ `setControl(true)`；配置时或运行中 `rcs_session_update` 都可以）。由本机执行时需要输入权限（macOS 的“辅助功能”；Windows 在用户会话里的进程就有），没有时是 `RCS_ERROR_PERMISSION_REQUIRED`。
2. SDK 通知观看端已获授权，两端都收到 `RCS_EVENT_CONTROL`（`rcs_control_info`：`allowed` 是宿主的设置，`granted` 是此刻是否真的能控制）。观看端等到 `granted` 再发输入。
3. 分享端的 `on_input` 在每个事件执行前看到它（调用不重叠，几个观看端的也不重叠，`viewer_id` 是发送者）：由本机执行时返回 0 就丢弃这个事件（例如只允许某个窗口在前台时接收控制）；`target = RCS_INPUT_HOST` 和 `host_input_types` 里的类型由宿主自己执行，返回值不起作用。
4. 撤销控制、观看端断线、会话停止时，SDK 自动释放所有被按住的键、鼠标按钮、触点和笔；宿主执行输入时会收到 `RCS_INPUT_RELEASE_ALL`（换执行者、控制被暂停或收回、观看端走了），要把自己按住的都放开。
5. 被控端锁屏、弹出 UAC 安全桌面，或者系统撤回了输入权限时，授权**暂停**（只对本机执行的输入）：按住的键被释放，观看端的 `granted` 变成 0，`send_input` 返回 `INVALID_STATE`；系统重新接受输入后自动恢复。跟随安全桌面时锁屏和 UAC 不再让它暂停（[16](16-unattended.md)）。
6. 单个事件被系统拒绝（例如前台是以管理员身份运行的窗口）时，只释放按键并记入输入统计的 `last_result`，授权保持。

## 观看端：发送输入，`rcs_session_send_input`

- 输入事件（`rcs_input_event`，用 `RCS_INPUT_EVENT_INIT` 初始化）不阻塞地进队列；连续的 `MOVE` 合并（只留最后一个），`MOVE_RELATIVE` 和 `SCROLL` 加起来，触摸和笔不合并。
- 队列满时排队的事件被换成一个“释放全部按键”，返回 `QUEUE_FULL`：这时应停止捕获输入。
- 没在运行、或分享端没有授权时返回 `INVALID_STATE`；分享会话调用是 `UNSUPPORTED`。`viewer_id` 必须是 0。
- **观看端窗口失去焦点时发一个 `RCS_INPUT_RELEASE_ALL`**，否则对方可能一直按着某个键。
- 坐标是 0..65535 的绝对坐标，覆盖被共享的东西（整个显示器、那一块区域、那个窗口现在的位置），`stream` 说明是哪一路画面上的。窗口坐标的换算用 `rcsdk/view_geometry.hpp`（[08](08-viewing.md#画面在窗口里的位置和坐标)）。共享窗口或应用时输入只进它（[07](07-sharing.md#共享什么rcs_capture_config)）。

| 类型 | 字段 | 说明 |
| --- | --- | --- |
| `RCS_INPUT_MOVE` | `x`、`y` | 指针移到这里 |
| `RCS_INPUT_MOVE_RELATIVE` | `dx`、`dy`（-10 000..10 000） | 相对移动，被控端的像素，给隐藏指针、读移动量的程序（游戏、三维视图）；共享显示器时像那台机器自己的鼠标，共享区域、窗口、应用时指针留在里面 |
| `RCS_INPUT_BUTTON` | `x`、`y`、`code`（`RCS_BUTTON_LEFT`、`RIGHT`、`MIDDLE`、`BACK`、`FORWARD`）、`down` | 按下、松开 |
| `RCS_INPUT_WHEEL` | `x`、`y`、`wheel_x`、`wheel_y`（整行，-1000..1000） | 有刻度的滚轮；正数是向右、向上 |
| `RCS_INPUT_SCROLL` | `x`、`y`、`wheel_x`、`wheel_y`（1/120 行，-120 000..120 000） | 平滑滚动（触控板、高精度滚轮）；不满一个系统单位的留到下一次 |
| `RCS_INPUT_KEY` | `code`（USB HID 用法码）、`down`、`repeat` | 按键的位置，打出什么由被控端的键盘布局决定。媒体键是 `RCS_KEY_CONSUMER \| 用法码` |
| `RCS_INPUT_TEXT` | `code`（一个 Unicode 标量值） | 一个字符原样输入，不管键盘布局和按住的键：输入法上屏的字、表情、粘贴成键入的一段文字 |
| `RCS_INPUT_TOUCH` | `x`、`y`、`code`（触点 0..9）、`down`、`pressure`（1..1024） | 触摸：第一次 `down = 1` 放下，之后移动，`down = 0` 抬起 |
| `RCS_INPUT_PEN` | `x`、`y`、`down`、`pressure`、`tilt_x`、`tilt_y`、`rotation`、`pen`（`RCS_PEN_BARREL`、`ERASER`、`LEAVE`） | 笔：悬停、接触、离开 |
| `RCS_INPUT_RELEASE_ALL` | — | 放开按住的一切 |
| `RCS_INPUT_SECURE_ATTENTION` | — | Ctrl+Alt+Del（Windows；不能靠发三个键产生，见 [16](16-unattended.md)） |

**对端执行哪些**：`rcs_capabilities.input_types` 是本机能执行的类型；观看端从 `rcs_peer_info.input_types` 得知分享端接受哪些（系统执行的加上宿主执行的 `host_input_types`；宿主执行全部输入时是全部），据此决定怎么发，比如对端不收触摸就自己把触摸换成鼠标。SDK 不替宿主换：系统执行不了的事件在 `rcs_input_stats.last_result` 里记为 `UNSUPPORTED`。

| 类型 | Windows | macOS | iOS、Android（分享端） |
| --- | --- | --- | --- |
| 指针、按钮、滚轮、平滑滚动、按键、文本 | 是 | 是 | 交给宿主（`RCS_INPUT_HOST`） |
| 相对移动 | 是 | 是 | 交给宿主 |
| 媒体键 | 是，另有后退、前进、刷新等浏览器键 | 是，另有亮度 | 交给宿主 |
| 触摸 | Windows 10 1809 起 | 否 | 交给宿主 |
| 笔 | Windows 10 1809 起 | 是（数位板事件） | 交给宿主 |
| Ctrl+Alt+Del | 条件见 [16](16-unattended.md) | 否 | 否 |

```c
/* 观看端：在 (x, y) 点一下左键（坐标已经换算成 0..65535） */
rcs_input_event e = RCS_INPUT_EVENT_INIT;
e.type = RCS_INPUT_BUTTON;
e.x = x;
e.y = y;
e.code = RCS_BUTTON_LEFT;
e.down = 1;
rcs_session_send_input(viewer, &e);
e.down = 0;
rcs_session_send_input(viewer, &e);

/* 输入法上屏的一个字 */
rcs_input_event t = RCS_INPUT_EVENT_INIT;
t.type = RCS_INPUT_TEXT;
t.code = 0x4F60; /* “你” */
rcs_session_send_input(viewer, &t);
```

## 键码：从平台的键码到 HID 用法码

`RCS_INPUT_KEY` 用的是 USB HID 键盘页的用法码（字母 A=4、回车=40、左 Ctrl=224、左 Shift=225、左 Alt=226、左 Command / Win=227），按键盘上的位置命名，不按“哪个系统的虚拟键”。宿主要把自己平台的键码换成它；SDK 不提供公开的换算函数，示例里有现成的表，可以照着用：

| 平台的键码 | 示例里的换算 |
| --- | --- |
| Windows 的扫描码（`WM_KEYDOWN` 的 `lParam` 16..24 位） | `examples/common/keyboard.hpp` 的 `hidFromWindowsScanCode` |
| macOS 的虚拟键码（`NSEvent.keyCode`） | `examples/common/keyboard.hpp` 的 `hidFromMacKeyCode` |
| Qt 的键（`QKeyEvent`，先用本机的扫描码 / 键码，没有时按 `Qt::Key`）和鼠标按钮 | `examples/qml-demo/qt_input.hpp` |
| Win32 窗口里的整套键鼠捕获 | `examples/windows/input.hpp` |
| iOS 外接键盘 | `UIKey.keyCode` 本来就是 HID 用法码，直接用 |
| 字符（软键盘、输入法） | 不换键码，发 `RCS_INPUT_TEXT` |

快捷键的习惯不同（Mac 的 Command 对 Windows 的 Ctrl）：对端是什么平台看 `rcs_peer_info.platform`，要不要把 Ctrl 映射成 Command 由宿主决定。

## 触屏和移动端

观看端在手机、平板上时，触屏怎么变成输入是宿主的事，现有的类型都够用：

- 点按发 `BUTTON`（长按可以当右键），直接点哪里就先发 `MOVE`。
- 触控板模式：手指移动发 `MOVE`（宿主自己记着指针在哪里）或 `MOVE_RELATIVE`。
- 两指拖动发 `SCROLL`，两指轻点可以当右键。
- 外接键盘发 `KEY`；软键盘打的字发 `TEXT`（输入法组字时先在本地显示，选定后才发），退格和换行发 `KEY`。
- Apple Pencil、手写笔发 `PEN`（对方收不收看 `rcs_peer_info.input_types`），也可以原样发 `TOUCH`。

示例 `examples/qml-demo/remote_video.cpp` 实现了直接点按（长按右键、两指滚动、两指轻点右键）、触控板模式、软键盘和笔。

分享端在 iOS、Android 上时，对方的输入交给宿主（`RCS_INPUT_HOST`）：iOS 只能在宿主自己的应用里执行（`examples/qml-demo/app_input.cpp`），Android 经宿主的无障碍服务在整个系统里执行（[06](06-quickstart-android.md#分享端整个屏幕和设备的声音)）。

## 输入法：远端的文本插入点

组字在观看端本机做（本地输入法看得到本地键盘和用户），上屏的结果用 `TEXT` 发。缺的一块是“远端此刻输入的文字会落在哪里”，好让本地输入法把候选窗放在那里：用文本插入点模块 `rcsdk.text-caret`（[15](15-modules.md#文本插入点rcsdktext-caret)）。
