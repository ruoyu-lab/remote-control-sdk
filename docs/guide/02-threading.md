# 02 回调、线程、生命周期，以及从其他语言调用

SDK 的回调都在 SDK 自己的线程上执行。写错线程规则是集成里最常见的问题（死锁、在已销毁的对象上回调、界面卡住），本页一次讲清楚。

## 回调在哪个线程

| 回调 | 线程 | 并发 |
| --- | --- | --- |
| `on_event`（`rcs_session_callbacks`） | SDK 的工作线程 | 调用之间**不重叠**；可能在 `rcs_session_start` 返回之前就开始 |
| `on_gpu_frame` / `on_frame` | 观看端：每路画面的解码线程；分享端（本地预览）：采集编码循环里，回调慢就拖慢采集 | 每路按顺序；两个回调只能注册一个（都注册是 `INVALID_ARGUMENT`），都不注册就不交付画面 |
| `on_audio` | 会话的声音线程（从不在连接的线程上：慢的回调只拖住声音）；分享端每个观看端的声音各在自己的线程上 | 每个来源按顺序 |
| `on_input`（分享端） | SDK 的线程 | 不重叠（几个观看端的也不重叠） |
| `on_data` | 读这个连接的线程：画面和输入排在它后面 | 不重叠（几个观看端的也不重叠） |
| `on_signal` | SDK 的线程 | 不重叠 |
| `on_viewer_join`（分享端） | SDK 的线程 | 不重叠；要马上返回，问用户就返回 `RCS_ADMIT_LATER` |
| 宿主的编码器（`rcs_host_encoder_config`） | 每路一个编码线程 | 同一路按顺序，不同路可能同时 |
| 宿主的解码器（`rcs_host_decoder_config`） | 每路一个解码线程 | 同上 |
| `on_video` / `on_audio`（`rcs_encoded_output_config`） | 分享端：编码器的线程 / 发那个观看端声音的线程；观看端：解码线程 / 声音线程 | 两种包来自不同线程，可能同时；每种按顺序 |
| `on_rate_report`（`rcs_rate_decider_config`） | 连接的网络线程 | 一次一份（几个观看端的也是） |
| `on_transport_send`（`rcs_host_transport_config`） | 连接的线程 | 同一个 `route` 一次一个（分享端不同观看端的可能同时）；里面可以直接调 `rcs_session_transport_receive` |
| `on_agent_attach`（`rcs_agent_link_config`） | 助手链接的线程 | 要马上返回 |
| 模块的回调（`rcs_module_definition`） | SDK 的各个线程 | 同一个模块的同一半一个接一个（[15](15-modules.md)） |
| 剪贴板的 `on_clipboard` | 收到的：读连接的线程；发出的：读剪贴板的线程 | 不重叠 |
| 文件传输的 `read` / `write` / `resume`（`target` HOST） | 模块自己的一个线程 | 一次一个 |
| 日志回调（`rcs_set_log_callback`） | 任何写日志的 SDK 线程 | 一次一个；它执行时挡着所有要写日志的线程 |
| 助手的 `on_event`（`rcs_agent_callbacks`） | SDK 的线程 | 一次一个 |
| 看护者的 `on_event`（`rcs_supervisor_callbacks`） | 看护者自己的线程 | 一次一个 |

## 回调里能做什么

- 所有回调都必须**很快返回、不抛异常、不阻塞等别的线程**。要在界面线程处理就派发过去（例如 Qt 的 `QMetaObject::invokeMethod`、`DispatchQueue.main.async`、`PostMessage`）。
- 回调里**可以**调用查询函数（状态、统计、信息）、`rcs_session_update`、`rcs_session_send_data`、`rcs_session_send_input`、`rcs_session_admit_viewer`、`rcs_session_transport_receive` 等动作。
- 回调里**不能**对同一个会话调用 `rcs_session_start`、`rcs_session_stop`、`rcs_session_destroy`：返回 `RCS_ERROR_CALLBACK_THREAD`。替换层的回调、助手和看护者的回调也一样。要“收到某个事件就停止”，把停止派发到自己的线程。
- 在 `on_event` 里调用的函数引起的事件（例如在里面接受一个文件传输），等这次 `on_event` 返回后在同一线程上依次送出，所以 `on_event` 仍然不重叠，也不会死锁。
- 日志回调里调用 `rcs_set_log_callback`、任何对象的 start / stop / destroy 或 `rcs_agent_wait` 都返回 `RCS_ERROR_CALLBACK_THREAD`（日志回调挡着所有写日志的线程，在里面等它们会永远等下去）。`rcs_log_in_callback()` 说当前是不是在日志回调的线程上。
- `rcs_session_stop` 返回后，这次运行的回调不会再来。

## 谁拥有内存

| 东西 | 规则 |
| --- | --- |
| `rcs_session_config`、扩展链、`rcs_session_callbacks` | `rcs_session_create` 复制；`rcs_session_update` 复制传入的扩展。调用之后宿主可以释放它们 |
| `rcs_device_identity_config.trusted_keys` 指向的公钥数组 | create / update 时复制 |
| 回调的 `user_data`、扩展里的 `user_data`（宿主编解码器、传输、码率决定、助手门卫……） | **归宿主，要活得比会话久** |
| 模块设置（`rcs_module_config.settings`） | SDK 复制 `size` 字节；设置里指向的东西（例如剪贴板 `on_clipboard` 的 `user_data`）归宿主，要活得比会话久 |
| 回调参数：`rcs_event`（含其中的字符串和 payload）、`rcs_frame`、`rcs_audio_frame`、`rcs_data_message`、`rcs_input_event`、`rcs_viewer_info`、信令文本、`rcs_log_record`、编码包、`rcs_rate_report`、模块收到的消息 | **借给回调**，只在回调期间有效：要留就复制 |
| GPU 帧（`rcs_gpu_frame`） | 借给回调；要留到回调之后就 `rcs_gpu_frame_retain`，用完 `rcs_gpu_frame_release`。留住的帧在会话停止后仍然有效。Android 上宿主同时最多留 8 帧，留满时新画面会等：画完就放手 |
| 宿主交给 SDK 的数据：`rcs_session_send_data`、`rcs_session_push_video_frame`、`rcs_session_push_audio`、`rcs_session_transport_receive`、`rcs_clipboard_send`、`rcs_session_send_input` | 调用时复制，返回后宿主就可以重用缓冲区 |
| `rcs_session_push_gpu_frame` 的 GPU 图像 | SDK 持有它直到编码完（宿主推完就可以放手） |
| `rcs_session_push_hardware_buffer`（Android） | 返回前就复制到 SDK 自己的缓冲区，宿主随后就能 `Image.close()` |
| `rcs_session_push_sample_buffer`（Apple） | 缓冲区仍是宿主的，SDK 留下它需要的 |

文字一律是 UTF-8、以 NUL 结尾。结构体里的定长字符数组放不下时截短（例如 `message[128]`）。

## 线程安全

- 除非头文件另有说明，所有函数都可以从任何线程调用。
- `rcs_session_destroy` 不能和对同一个会话的其他调用同时进行：宿主保证 destroy 之后不再有人用这个句柄。
- 宿主主线程上的要求只有几个：macOS 上弹权限提示（`rcs_request_screen_capture_permission`、`rcs_request_input_permission`、`rcs_request_microphone_permission`，不在主线程是 `RCS_ERROR_WRONG_THREAD`）。没有界面的 macOS 宿主要让主线程的 run loop 转着（`CFRunLoopRun` 或 `CFRunLoopRunInMode`），系统框架会往上面投递工作。

## C++ 包装的规则

- `rcsdk::Callbacks` 里的 `std::function` 由 `noexcept` 的跳板调用：异常逃出回调会让进程终止。
- `rcsdk::Session` 析构时调用 `rcs_session_destroy`（停止并等所有回调结束）；在它自己的回调里析构是编程错误，进程终止。
- 复制 `rcsdk::GpuFrame` 就增加一次引用（retain），析构时放手。
- 替换层的接口类（`HostEncoder`、`HostDecoder`、`EncodedOutput`、`RateDecider`、`HostTransport`、`AgentGate`）的对象要活得比会话久。

## 从其他语言调用

C 接口（`rcsdk.h` 等头文件）是稳定的 C ABI，任何能调用 C 的语言都能用。要注意的几点：

- **结构体**：按 C 的布局声明（顺序、对齐不改），每个结构体的第一个字段 `struct_size`（扩展是 `header.struct_size`）必须等于本版本 C 的 `sizeof`，否则 `INVALID_ARGUMENT`。最好由绑定生成器（Rust 的 bindgen、C# 的 ClangSharp、Python 的 ctypesgen）从头文件生成。
- **初始化**：每个 `*_INIT` 宏都有一个同名的小写函数（`RCS_SESSION_CONFIG_INIT` → `rcs_session_config_init()`），返回同样的值，`RCSDK_VERSION` 是 `rcs_header_version()`。它们是头文件里的 `static inline` 函数，库里没有这个符号：Swift 直接调用；靠头文件生成绑定的（bindgen、ClangSharp）按各自的方式包装 inline 函数，或者在宿主里编一个调用它们的小 C 文件。不要逐个字段照抄宏的值（以后版本的默认值可能变）。
- **枚举**是 `uint32_t` 的 typedef 加匿名 `enum` 常量，结果码 `rcs_result` 是 `int32_t`；布尔是 `uint32_t` 的 0 / 1。
- **`rcs_event` 有一个匿名 union**：按 `type` 读对应的成员（`RCS_EVENT_STATE` → `state`……）。
- **回调**是普通的 C 函数指针加 `void* user_data`，在 SDK 自己的线程上调用：托管语言要让回调对象活着（不被回收），并按语言的要求把线程接入运行时（Java 的 `AttachCurrentThread`、.NET 的 `UnmanagedCallersOnly`）。
- **扩展链**存的是指针：传给 `rcs_session_create` 的那一刻每个扩展必须在内存里完整、地址稳定（托管语言要固定住或放在非托管内存里）。

### Swift（iOS、macOS）

在 Swift 6.3 上用 SDK 的头文件编过、并实际运行过（一个 Swift 写的观看端连上本机的 C 分享端，5 秒解码 280 帧）：

- 函数、结构体、typedef、枚举常量都能导入。匿名 `enum` 的常量导入成 `Int`：赋给字段时转一下，例如 `rcs_role(RCS_ROLE_VIEW)`；`== RCS_OK` 这样的比较可以直接写。
- `*_INIT` 宏和 `RCSDK_VERSION` 导入不了，用它们的函数：`rcs_session_config_init()`、`rcs_session_callbacks_init()`、`rcs_decoder_config_init()`……，`rcs_header_version()`。
- 用 XCFramework 时 `import RemoteControlSDK` 就有全部头文件；直接用头文件时给模块写一个 module map，并让 Clang 找得到 `<rcsdk/...>`（`swiftc -I <module map 所在目录> -Xcc -I<SDK>/include`）。

```swift
import RemoteControlSDK

final class Viewer {
    var session: OpaquePointer? = nil
    func onEvent(_ e: UnsafePointer<rcs_event>) {
        if e.pointee.type == rcs_event_type(RCS_EVENT_STATE) {   // union 的成员直接是属性
            print("state", e.pointee.state.value, e.pointee.state.result)
        }
    }
}

let viewer = Viewer()
guard rcs_version_number() == rcs_header_version() else { fatalError("库和头文件不是同一个版本") }
var config = rcs_session_config_init()
withUnsafeMutableBytes(of: &config.connect.address) { buffer in   // 定长的 char 数组导入成元组
    buffer.copyBytes(from: Array("192.168.1.20".utf8) + [0])
}
var key = [UInt8](repeating: 0, count: 32)
_ = rcs_generate_connection_key(&key, 32)   // 真实的宿主从自己的通道拿到分享端的密钥
withUnsafeMutableBytes(of: &config.identity.key) { $0.copyBytes(from: key) }

var callbacks = rcs_session_callbacks_init()
callbacks.user_data = Unmanaged.passUnretained(viewer).toOpaque()   // viewer 要活得比会话久
callbacks.on_event = { event, userData in   // 不捕获任何东西的闭包就是 C 函数指针
    Unmanaged<Viewer>.fromOpaque(userData!).takeUnretainedValue().onEvent(event!)
}
callbacks.on_gpu_frame = { frame, _ in
    var info = rcs_gpu_frame_info_init()
    if rcs_gpu_frame_get_info(frame, &info) == RCS_OK {
        _ = info.resource   // CVPixelBufferRef：交给 Metal 渲染（CVMetalTextureCache）
    }
}
var decoder = rcs_decoder_config_init()
decoder.codecs = (rcs_codec(RCS_CODEC_HEVC), rcs_codec(RCS_CODEC_H264), 0, 0, 0, 0, 0, 0)
// 扩展链：把整个结构体的地址当作它开头的 rcs_extension 传进去（SDK 按 struct_size 读整个结构体）。
// 不要写 &decoder.header：Swift 可能只传 header 的一份临时拷贝。
let created: rcs_result = withUnsafeMutablePointer(to: &decoder) { d in
    config.extensions = UnsafePointer(UnsafeMutableRawPointer(d).assumingMemoryBound(to: rcs_extension.self))
    defer { config.extensions = nil }
    return rcs_session_create(&config, &callbacks, &viewer.session)   // create 复制整条链
}
if created != RCS_OK {
    var reason = [CChar](repeating: 0, count: 256)
    _ = rcs_get_last_call_error(&reason, 256)
    print(String(cString: reason))
}
```

同样的道理适用于 `rcs_preset_defaults` 这类写入整个扩展的函数：传整个结构体的地址，不要传 `&ext.header`。
