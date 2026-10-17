# 06 快速开始：Android

SDK 可以编进 Android 应用（Android 10 / API 29 起，arm64-v8a；x86_64 给模拟器和 Chromebook，只编译过）。两种角色都能当：观看端看、控 Mac 和 Windows；分享端分享整个屏幕和设备的声音，经无障碍服务被控。Android 只让 Java 的 MediaProjection 采集屏幕、只让无障碍服务操作别的应用，而 SDK 是不带 Java 代码的原生库，所以分享端的画面和设备的声音由宿主推（`RCS_CAPTURE_HOST`、`RCS_AUDIO_HOST`），输入交给宿主执行（`RCS_INPUT_HOST`）。SDK 的测试在 Samsung Galaxy S25+（Android 16）上通过；应用里要人操作的部分（录屏、设备声音、无障碍控制、回声消除）还没在真机上验证（见 [ROADMAP](../ROADMAP.md)）。

## 引入 SDK

**AAR（用 Gradle 的宿主）**：`scripts/build-android-aar.sh [输出目录]`（默认 `build/aar`，要 `zip` 和 NDK）打出 `RemoteControlSDK-<版本>.aar`：每种 ABI（arm64-v8a、x86_64）一个静态库，核心、BearSSL、编进 SDK 的编解码库和全部官方模块合在里面，加头文件，是一个 Prefab 包，清单带 `INTERNET`、`ACCESS_NETWORK_STATE` 和 `WAKE_LOCK`。x86_64 的编解码库要 NASM 编汇编；没有 NASM 时只打 arm64-v8a：`scripts/build-android-aar.sh build/aar arm64-v8a`。

```groovy
// app/build.gradle
android {
    defaultConfig { minSdk 29; ndk { abiFilters 'arm64-v8a', 'x86_64' } }   // x86_64：模拟器、Chromebook
    buildFeatures { prefab true }
}
dependencies {
    implementation files('libs/RemoteControlSDK-0.1.6-beta.aar')   // 或放进自己的 Maven 仓库
}
```

```cmake
# 宿主原生部分的 CMakeLists.txt
find_package(RemoteControlSDK REQUIRED CONFIG)
add_library(app SHARED rcsdk_jni.c)
target_link_libraries(app PRIVATE RemoteControlSDK::RemoteControl)   # 要链接的系统库随包带上
```

ndk-build：`LOCAL_STATIC_LIBRARIES := RemoteControl`，再 `$(call import-module,prefab/RemoteControlSDK)`。宿主的 C++ 库 `c++_static` 或 `c++_shared` 都行。

**不用 Gradle**：预设 `android-arm64` 或 `android-x86_64`（在 macOS 或 Linux 上用 NDK 交叉编译，NDK r27 测过；x86_64 要 NASM）编出静态库（`librcsdk_core.a`、BearSSL、编解码库 `librcsdk_libvpx.a`、`librcsdk_dav1d.a`、`librcsdk_libaom.a` 和官方模块），宿主链进自己的原生库，再链接 `mediandk`、`android`、`aaudio`、`EGL`、`GLESv3`、`log`。也可以把 SDK 当 CMake 子项目编进宿主（例如 Qt 的 `qt-cmake`）：`ANDROID_PLATFORM` 至少 `android-29`（Qt 的工具链默认 `android-28`，要写明），更低时配置就报错。

- 密码学和 TURN over TLS 用编进 SDK 的 BearSSL（Android 没有系统的原生密码学接口）；arm64 上画面、声音和数据加密用的 AES-CTR、SHA-256 是处理器的 ARMv8 指令（没有这些指令的处理器用 BearSSL 的软件）。
- 宿主不是应用、是经 adb 或 init 跑的原生程序时也能用：SDK 在第一次用 MediaCodec 前启动本进程的 binder 线程池。

## 交出 Java VM：`rcs_android_initialize`

有几样东西 Android 只给 Java：麦克风权限的查询、声音设备的列表、Wi-Fi 不省电（`WifiManager`）、网络的种类和计费（`ConnectivityManager`）、剪贴板模块（`ClipboardManager`）、保持唤醒模块（`PowerManager`）。宿主在用到它们之前调用一次 `rcs_android_initialize(java_vm, context)`；没调用时这几样以原因报告 `UNSUPPORTED`，其余照常。调用时 SDK 还在后台线程里先把编解码器列好（逐个打开、解码探针，S25+ 上约 0.6 秒），第一次连接就不用等。模块用 `rcs_android_get_java` 取同一个 VM 和应用的 Context。

SDK 不带 Java 代码，宿主写一个小小的 JNI 桥（下面的 C 文件用 NDK r27 的 clang 检查过语法；Kotlin 部分没有编译检查）：

```c
/* rcsdk_jni.c：宿主自己的 JNI 桥（Kotlin 一侧见下面）。SDK 不带 Java 代码。 */
#include <jni.h>
#include <android/hardware_buffer_jni.h>
#include <rcsdk/android.h>
#include <rcsdk/rcsdk.h>
#include <stdint.h>

/* Kotlin：external fun nativeInit(context: Context)；在用 SDK 之前调用一次（例如 Application.onCreate）。 */
JNIEXPORT void JNICALL Java_com_example_remote_Sdk_nativeInit(JNIEnv* env, jclass cls, jobject context) {
    (void)cls;
    JavaVM* vm = NULL;
    (*env)->GetJavaVM(env, &vm);
    rcs_android_initialize(vm, context); /* SDK 留下应用的 Context（全局引用）；可以重复调用 */
}

/* Kotlin：external fun nativePushPicture(session: Long, buffer: HardwareBuffer, rotation: Int): Int */
JNIEXPORT jint JNICALL Java_com_example_remote_Sdk_nativePushPicture(JNIEnv* env, jclass cls, jlong session,
                                                                     jobject hardwareBuffer, jint rotation) {
    (void)cls;
    AHardwareBuffer* buffer = AHardwareBuffer_fromHardwareBuffer(env, hardwareBuffer);
    rcs_hardware_buffer_info info = RCS_HARDWARE_BUFFER_INFO_INIT;
    info.rotation = (uint32_t)rotation; /* MediaProjection 的画面是正的：0 */
    /* 返回之前 SDK 已经把画面复制到自己的缓冲区：Kotlin 一侧随后就 image.close() */
    return rcs_session_push_hardware_buffer((rcs_session)(intptr_t)session, buffer, &info);
}

/* Kotlin：external fun nativePushSound(session: Long, samples: FloatArray, frames: Int): Int
 * AudioPlaybackCapture 的 AudioRecord 读出的 48 kHz 立体声 float（交错）。 */
JNIEXPORT jint JNICALL Java_com_example_remote_Sdk_nativePushSound(JNIEnv* env, jclass cls, jlong session,
                                                                   jfloatArray samples, jint frames) {
    (void)cls;
    jfloat* data = (*env)->GetFloatArrayElements(env, samples, NULL);
    rcs_audio_frame frame = RCS_AUDIO_FRAME_INIT; /* 48 kHz、2 声道 */
    frame.frames = (uint32_t)frames;
    frame.samples = data;
    const rcs_result result = rcs_session_push_audio((rcs_session)(intptr_t)session, &frame); /* 复制 */
    (*env)->ReleaseFloatArrayElements(env, samples, data, JNI_ABORT);
    return result;
}
```

会话本身（`rcs_session_create`、`start`、回调）照常用 C 或 C++ 写在原生部分（[01](01-concepts.md#最小的完整程序)），句柄以 `long` 交给 Kotlin。回调在 SDK 的线程上：要调 Java 就先 `AttachCurrentThread`（[02](02-threading.md)）。

## 权限清单

`AndroidManifest.xml` 按用到的写：

| 权限 | 用途 |
| --- | --- |
| `INTERNET` | 连网（AAR 已带） |
| `ACCESS_NETWORK_STATE` | 网络的种类、计费和默认网络（`rcs_list_networks`、统计和路径事件里的网络、`excluded_networks`），套接字绑定到一块网络（AAR 已带） |
| `ACCESS_WIFI_STATE` | 所连 Wi-Fi 的频段、信道、信号和速率（`rcs_list_wifi`、`RCS_EVENT_STALLS` 里的 Wi-Fi）（AAR 已带） |
| `WAKE_LOCK` | 连上期间 Wi-Fi 不省电（`wifi_low_latency`）、保持唤醒模块（AAR 已带） |
| `RECORD_AUDIO` | 麦克风；AudioPlaybackCapture 录设备的声音也要它。运行时由宿主的 Activity 申请（`requestPermissions`），SDK 只报告状态 |
| `FOREGROUND_SERVICE`、`FOREGROUND_SERVICE_MEDIA_PROJECTION`、`POST_NOTIFICATIONS` | 分享屏幕的前台服务和它的通知 |
| `CHANGE_WIFI_MULTICAST_STATE` | 宿主自己的局域网发现（组播锁）；SDK 不用 |

再加上前台服务（`android:foregroundServiceType="mediaProjection"`）和无障碍服务（被控时）的声明。示例：`examples/qml-demo/android/AndroidManifest.xml`。

## 观看端

- MediaCodec 解码（硬件，或 Android 自带的软件解码器，`rcs_list_codec_implementations` 用 MediaCodec 的名字列出，后端 `RCS_BACKEND_MEDIACODEC`），解码器直接画进 `AImageReader` 的硬件缓冲区。解码器收哪些画面格式（10 位、4:4:4）由 SDK 解一次探针关键帧试出；S25+ 上高通的 HEVC、AV1 解码器收 10 位，4:4:4 不收。
- GPU 帧是 `AHardwareBuffer*`（`RCS_GPU_AHARDWAREBUFFER`，解码器自己的排列，可能分块或压缩；`format` 说它装的画面：NV12、P010、NV24、P410）。做成 `EGLImage`，当 `GL_TEXTURE_EXTERNAL_OES` 采样，GPU 按缓冲区的数据空间换算颜色；Vulkan 也可以导入。**帧持有解码器的图像，宿主同时最多留 8 帧**：画完就放手，一直拿着新画面会等（日志说明）。

```c
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <rcsdk/gpu.h>

/* 扩展函数在运行时取（不必链接扩展的导出）。 */
static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC getNativeClientBuffer;
static PFNEGLCREATEIMAGEKHRPROC createImage;
static PFNEGLDESTROYIMAGEKHRPROC destroyImage;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture;

static void load_extensions(void) {
    getNativeClientBuffer =
        (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    createImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    destroyImage = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    imageTargetTexture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
}

/* 渲染线程（当前有 GL 上下文），帧已经 rcs_gpu_frame_retain：把 AHardwareBuffer 绑成外部纹理。
 * 着色器：#extension GL_OES_EGL_image_external : require，uniform samplerExternalOES picture;
 * 采样直接得到 RGB（GPU 按缓冲区的数据空间换算 YCbCr）。画完、GPU 用完后 destroyImage，
 * 再 rcs_gpu_frame_release：解码器只有几张图像，留着不放新画面就会等。 */
static EGLImageKHR bind_frame(rcs_gpu_frame frame, GLuint texture) {
    rcs_gpu_frame_info info = RCS_GPU_FRAME_INFO_INIT;
    if (rcs_gpu_frame_get_info(frame, &info) != RCS_OK || info.resource_type != RCS_GPU_AHARDWAREBUFFER) {
        return EGL_NO_IMAGE_KHR;
    }
    const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR image = createImage(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                    getNativeClientBuffer((const struct AHardwareBuffer*)info.resource), attributes);
    if (image != EGL_NO_IMAGE_KHR) {
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
        imageTargetTexture(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)image);
    }
    return image; /* 只画 info.width × info.height 的部分（表面可能更大） */
}
```

- 要 CPU 帧（`on_frame`）时 SDK 转成 BGRA8（CPU 映射得了的缓冲区在 CPU 上转，其余由 SDK 的 GL 线程画成 BGRA8）。示例：`examples/qml-demo/video_node_gles.cpp`（Qt Quick 的 OpenGL ES 渲染节点直接画外部纹理）。
- **前后台**：Android 上进了后台也能编解码，SDK 不观察应用的状态（`RCS_EXT_BACKGROUND` 不起作用）；宿主按自己的生命周期调用 `rcs_session_pause`。
- 触屏变成输入和 iOS 一样由宿主做（[11](11-input.md#触屏和移动端)）。

## 分享端：整个屏幕和设备的声音

1. 宿主用 MediaProjection 共享屏幕：用户在系统的对话框里同意（每次都问；Android 14 起用户也可以只共享一个应用），系统要求在 `mediaProjection` 类型的前台服务里拿 MediaProjection、先注册回调再建虚拟显示器。
2. 虚拟显示器画进 `ImageReader`（`RGBA_8888`，用途 `HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE`，API 29 的 `ImageReader.newInstance(w, h, format, maxImages, usage)`）。每张 `Image` 的 `hardwareBuffer` 经上面的 JNI 交给 `rcs_session_push_hardware_buffer`：SDK 在 GPU 上转正、按原来的形状放进流的尺寸，函数返回前就画完，宿主随后 `image.close()`。流的尺寸按屏幕的形状定，转屏时用 `rcs_session_update` 换（SDK 按原来的形状放进任何尺寸）。编码用 MediaCodec 的输入 Surface，SDK 的 GL 线程把画面画上去，不经 CPU；编码只出 4:2:0 8 位。
3. 设备放的声音：AudioPlaybackCapture（`AudioRecord`，48 kHz 立体声 float，可以 `excludeUid` 排除自己）读出来，经 JNI 交给 `rcs_session_push_audio`（发送声音的来源 `RCS_AUDIO_HOST`）。
4. 被控：`setControl(true, RCS_INPUT_HOST)`（C：`rcs_control_config.target = RCS_INPUT_HOST`），输入交给 `on_input`；宿主自己的无障碍服务（用户在“设置 › 无障碍”里打开；Samsung 是“设置 › 辅助功能 › 已安装的应用”）执行：`dispatchGesture` 做点按、长按、拖动（`continueStroke` 一段一段接上）、滑动，`performGlobalAction` 做返回、主屏幕、最近任务，文字经有焦点输入框的 `ACTION_SET_TEXT`。坐标是画面上的 0..65535，按 `rcs_stream_info.content_*` 去掉黑边再按屏幕的实际像素换算（[11](11-input.md)）。

```kotlin
// 前台服务里（MediaProjection 已经拿到）；没有编译检查，按 Android API 29 的文档写。
val reader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 3,
                                     HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE)
reader.setOnImageAvailableListener({ r ->
    r.acquireLatestImage()?.use { image ->
        image.hardwareBuffer?.use { buffer -> Sdk.nativePushPicture(session, buffer, 0) }
    }   // use { } 结束时 image.close()：SDK 已经复制完
}, handler)
projection.createVirtualDisplay("rcsdk", width, height, densityDpi, 0, reader.surface, null, handler)
```

示例：`examples/qml-demo/android/src/local/rcsdk/qmldemo/ScreenCaptureService.java`、`RemoteInputService.java` 和 `examples/qml-demo/screen_projection_android.cpp`。

## 声音和网络

- 播放用 AAudio（低延迟），输出设备变了自动重开。麦克风是 AAudio 的输入：`voice_processing`（默认 1）用 `VOICE_COMMUNICATION` 预设，由系统消回声、降噪；关掉时用 `VOICE_RECOGNITION`。`rcs_list_audio_devices` 列出 `AudioManager.getDevices` 的设备（需要 Java VM），`device` 是 `AudioDeviceInfo.getId()`。AAC 用 MediaCodec 的编码器，只有 96、128、160、192 kbit/s 四档。
- 连上期间 SDK 拿低延迟的 Wi-Fi 锁（`WIFI_MODE_FULL_LOW_LATENCY`，`rcs_network_tuning_config.wifi_low_latency`），要 `WAKE_LOCK` 权限和交出的 Java VM；Android 只在应用在前台、亮屏时让它生效。限定网卡（`local_interface`、`rcs_paths_config` 的网络选择）在交出 Java VM 时按网络标记套接字（`android_setsocknetwork`），否则用 `SO_BINDTODEVICE`（Linux 5.7 起的内核；Android 是否照它路由没有实测）。
- 换网络（[09](09-transports.md#网络变化连接迁移)）：Android 不把网络变化告诉原生代码，SDK 在有会话运行时每半秒看一次网卡（交出 Java VM 时连同 `ConnectivityManager` 的默认网络和计费）；宿主自己的 `ConnectivityManager.NetworkCallback` 可以在 `onAvailable`、`onLost`、`onCapabilitiesChanged` 里调 `rcs_notify_network_change`，早几百毫秒。没有绑定、没有 `connect` 的 UDP 套接字跟着默认网络走（netd 只在 `connect` 时给套接字打网络标记），所以 Wi-Fi 换到蜂窝时连接自己跟过去；宿主调了 `bindProcessToNetwork` 时不会。

## 模块在 Android 上

- `rcsdk.clipboard` 读写系统剪贴板，只有文字和 HTML（图片在 Android 上是别的应用的内容 URI，模块不收发）。Android 10 起只有有焦点的前台应用能读剪贴板，在后台时看不到新内容；写不受限。复制时标了 `EXTRA_IS_SENSITIVE` 的内容按隐藏处理。
- `rcsdk.power` 用唤醒锁：`SYSTEM` 是 `PARTIAL_WAKE_LOCK`，`DISPLAY` 让屏幕常亮；要 `WAKE_LOCK` 权限。
- `rcsdk.text-caret`、`rcsdk.thumbnail` 报告不可用；`rcsdk.file-transfer` 照常可用。
