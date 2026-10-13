// Sound on Windows: capture with WASAPI loopback, microphones with WASAPI
// capture endpoints, playback with WASAPI shared mode, AAC with the Media
// Foundation encoder and decoder.
//
// Capture: leaving this application's sounds out needs process loopback
// (Windows 10 build 20348 / Windows 11): a virtual device that mixes every
// process except this one's tree. Otherwise plain loopback of the default
// output device. Capture and playback each run on a thread of their own that
// reopens the stream when the device changes or fails, reporting the gap.
#include "platform/windows/media_foundation.hpp"
#include "platform/windows/win.hpp"
#include "platform/platform.hpp"
#include "util/log.hpp"

#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

namespace rcsdk::platform {
namespace {

using win::check;
using win::Com;
using win::ComThread;

// ---- Process loopback declarations (audioclientactivationparams.h, missing from MinGW) ----

enum ActivationType { activationProcessLoopback = 1 };
enum LoopbackMode { excludeTargetProcessTree = 1 };
struct ProcessLoopbackParams {
    DWORD targetProcessId;
    LoopbackMode mode;
};
struct ActivationParams {
    ActivationType type;
    ProcessLoopbackParams loopback;
};
constexpr wchar_t processLoopbackDevice[] = L"VAD\\Process_Loopback";

// {32D186A7-218F-4C75-8876-DD77273A8999}, missing from MinGW's wmcodecdsp.h.
constexpr GUID aacDecoderClsid = {0x32d186a7, 0x218f, 0x4c75, {0x88, 0x76, 0xdd, 0x77, 0x27, 0x3a, 0x89, 0x99}};
constexpr PROPERTYKEY friendlyNameKey = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
                                         14};

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT.
constexpr GUID floatSubtype = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

constexpr auto retryInterval = 2s;
constexpr REFERENCE_TIME streamBuffer = 200'000; // 20 ms, in 100 ns units.

uint32_t windowsBuild() {
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (const auto function = reinterpret_cast<RtlGetVersion>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))) {
        function(&info);
    }
    return info.dwBuildNumber;
}

WAVEFORMATEX waveFormat(bool floating) {
    WAVEFORMATEX f{};
    f.wFormatTag = floating ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    f.nChannels = audioChannels;
    f.nSamplesPerSec = audioRate;
    f.wBitsPerSample = floating ? 32 : 16;
    f.nBlockAlign = WORD(f.nChannels * f.wBitsPerSample / 8);
    f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
    return f;
}

// MMCSS "Pro Audio" scheduling for a capture or playback thread: woken on
// time even while games or encoders load the CPU.
class AudioThreadPriority {
public:
    AudioThreadPriority() {
        DWORD index = 0;
        handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
    }
    ~AudioThreadPriority() {
        if (handle_) {
            AvRevertMmThreadCharacteristics(handle_);
        }
    }

private:
    HANDLE handle_ = nullptr;
};

struct Endpoint {
    Com<IMMDevice> device;
    std::wstring id;
    std::string name;
};

Com<IMMDeviceEnumerator> deviceEnumerator() {
    Com<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(enumerator.out())))) {
        return {};
    }
    return enumerator;
}

// A device's id and name.
Endpoint describe(Com<IMMDevice> device) {
    Endpoint result;
    result.device = std::move(device);
    LPWSTR id = nullptr;
    if (SUCCEEDED(result.device->GetId(&id))) {
        result.id = id;
        CoTaskMemFree(id);
    }
    Com<IPropertyStore> properties;
    if (SUCCEEDED(result.device->OpenPropertyStore(STGM_READ, properties.out()))) {
        PROPVARIANT name;
        PropVariantInit(&name);
        if (SUCCEEDED(properties->GetValue(friendlyNameKey, &name)) && name.vt == VT_LPWSTR) {
            result.name = name.pwszVal ? win::narrow(name.pwszVal) : std::string();
        }
        PropVariantClear(&name);
    }
    return result;
}

// The default device of `flow` for `role`, or none.
std::optional<Endpoint> defaultEndpoint(EDataFlow flow, ERole role) {
    const auto enumerator = deviceEnumerator();
    Com<IMMDevice> device;
    if (!enumerator || FAILED(enumerator->GetDefaultAudioEndpoint(flow, role, device.out()))) {
        return std::nullopt;
    }
    return describe(std::move(device));
}

std::optional<Endpoint> defaultOutput() {
    return defaultEndpoint(eRender, eConsole);
}

// A device as the SDK numbers it (rcs_audio_device_info.device_id): a hash of
// the system's id string, the same while the device stays installed; never 0.
uint32_t numberOf(const std::wstring& id) {
    uint32_t hash = 2166136261u;
    for (const wchar_t c : id) {
        hash = (hash ^ uint32_t(c)) * 16777619u;
    }
    return hash ? hash : 1;
}

// The active devices of `flow`.
std::vector<Endpoint> endpoints(EDataFlow flow) {
    std::vector<Endpoint> result;
    const auto enumerator = deviceEnumerator();
    Com<IMMDeviceCollection> devices;
    UINT count = 0;
    if (!enumerator || FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, devices.out())) ||
        FAILED(devices->GetCount(&count))) {
        return result;
    }
    for (UINT i = 0; i < count; ++i) {
        Com<IMMDevice> device;
        if (SUCCEEDED(devices->Item(i, device.out()))) {
            result.push_back(describe(std::move(device)));
        }
    }
    return result;
}

// Tells when the default output device changes or a device comes or goes:
// the playback and capture loops check a flag instead of asking the system,
// which can take longer than a 3 ms period.
class DeviceWatcher final : public IMMNotificationClient {
public:
    // Watching the default device of `flow` for `role`.
    explicit DeviceWatcher(EDataFlow flow = eRender, ERole role = eConsole) : flow_(flow), role_(role) {
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator_.out())))) {
            registered_ = SUCCEEDED(enumerator_->RegisterEndpointNotificationCallback(this));
        }
    }
    ~DeviceWatcher() {
        if (registered_) {
            enumerator_->UnregisterEndpointNotificationCallback(this);
        }
    }
    DeviceWatcher(const DeviceWatcher&) = delete;
    DeviceWatcher& operator=(const DeviceWatcher&) = delete;

    // True once after a change.
    bool changed() noexcept { return changed_.exchange(false); }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == flow_ && role == role_) {
            changed_ = true;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        changed_ = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override {
        changed_ = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
    // Owned by its creator, not reference counted.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

private:
    const EDataFlow flow_;
    const ERole role_;
    Com<IMMDeviceEnumerator> enumerator_;
    bool registered_ = false;
    std::atomic<bool> changed_{false};
};

// Completion of ActivateAudioInterfaceAsync: free-threaded, signals an event.
class Activation final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    Activation() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~Activation() { CloseHandle(done_); }
    HANDLE done() const noexcept { return done_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (iid == __uuidof(IAgileObject)) {
            *object = static_cast<IAgileObject*>(this);
        } else {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = --references_;
        if (!count) {
            delete this;
        }
        return count;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(done_);
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
    HANDLE done_;
};

// An audio client capturing everything but this process's tree.
Com<IAudioClient> processLoopbackClient() {
    ActivationParams params{activationProcessLoopback, {GetCurrentProcessId(), excludeTargetProcessTree}};
    PROPVARIANT activation{};
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(params);
    activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    Com<Activation> handler;
    *handler.out() = new Activation();
    Com<IActivateAudioInterfaceAsyncOperation> operation;
    check(ActivateAudioInterfaceAsync(processLoopbackDevice, __uuidof(IAudioClient), &activation, handler.value,
                                      operation.out()),
          "Activating process loopback", RCS_ERROR_CAPTURE);
    if (WaitForSingleObject(handler->done(), 5000) != WAIT_OBJECT_0) {
        fail(RCS_ERROR_CAPTURE, "Activating process loopback timed out");
    }
    HRESULT result = E_FAIL;
    Com<IUnknown> client;
    check(operation->GetActivateResult(&result, client.out()), "Process loopback", RCS_ERROR_CAPTURE);
    check(result, "Process loopback", RCS_ERROR_CAPTURE);
    return client.as<IAudioClient>("Process loopback client", RCS_ERROR_CAPTURE);
}

bool processLoopbackAvailable() {
    static const bool available = windowsBuild() >= 20348;
    return available;
}

// Runs `open` and `serve` on a thread of its own until stopped: when either
// throws, the problem is reported and both run again after a pause.
class Restarter {
public:
    using Report = std::function<void(const AudioStatus&)>;
    ~Restarter() { stop(); }

    // `serve` returns normally to reopen (e.g. another default device).
    // `processing`: whether a running stream has voice processing (AudioStatus).
    void start(std::function<void()> open, std::function<void()> serve, std::function<void()> close, Report report,
               std::function<bool()> processing = {}) {
        thread_ = log::thread([=, this] {
            ComThread com;
            AudioThreadPriority priority;
            std::optional<AudioStatus> reported;
            const auto publish = [&](const AudioStatus& now) {
                if (!reported || reported->active != now.active || reported->message != now.message ||
                    reported->voiceProcessing != now.voiceProcessing) {
                    reported = now;
                    if (!now.active) {
                        log::warning("audio", now.message);
                    }
                    report(now);
                }
            };
            while (!stopping()) {
                try {
                    open();
                    publish({true, RCS_OK, {}, processing && processing()});
                    serve();
                    close();
                    continue;
                } catch (const Failure& failure) {
                    close();
                    publish({false, failure.result(), failure.what()});
                }
                std::unique_lock lock(mutex_);
                wake_.wait_for(lock, retryInterval, [&] { return stopping_; });
            }
        });
    }
    void stop() noexcept {
        {
            std::lock_guard lock(mutex_);
            if (stopping_ || !thread_.joinable()) {
                return;
            }
            stopping_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }
    bool stopping() {
        std::lock_guard lock(mutex_);
        return stopping_;
    }

private:
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
};

// ---- Capture --------------------------------------------------------------------------

class WasapiLoopbackCapture final : public AudioCapture {
public:
    ~WasapiLoopbackCapture() override { stop(); }

    uint32_t periodFrames() const noexcept override { return period_.load(); }

    void start(const AudioCaptureConfig& config, Deliver deliver, Status status) override {
        exclude_ = config.excludeCurrentProcess;
        deliver_ = std::move(deliver);
        restarter_.start([this] { open(); }, [this] { serve(); }, [this] { close(); }, std::move(status));
    }
    void stop() noexcept override { restarter_.stop(); }

private:
    void open() {
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (exclude_) {
            if (!processLoopbackAvailable()) {
                fail(RCS_ERROR_UNSUPPORTED, "Leaving this application's sound out needs Windows 10 build 20348 or "
                                            "Windows 11; set exclude_current_process to 0 to share it too");
            }
            client_ = processLoopbackClient();
            float_ = false; // Process loopback delivers 16-bit PCM.
            flags &= ~DWORD(AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY);
            deviceId_.clear();
        } else {
            auto device = defaultOutput();
            if (!device) {
                fail(RCS_ERROR_DEVICE, "No output device to capture sound from");
            }
            check(device->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(client_.out())),
                  "Opening the output device for capture", RCS_ERROR_DEVICE);
            float_ = true;
            deviceId_ = device->id;
        }
        const auto format = waveFormat(float_);
        check(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, streamBuffer, 0, &format, nullptr),
              "Starting loopback capture", RCS_ERROR_CAPTURE);
        check(client_->SetEventHandle(event_), "Loopback capture events", RCS_ERROR_CAPTURE);
        UINT32 bufferFrames = 0;
        REFERENCE_TIME latency = 0;
        client_->GetBufferSize(&bufferFrames);
        client_->GetStreamLatency(&latency);
        log::debug("audio", std::string(exclude_ ? "Process" : "Device") + " loopback buffer " +
                                std::to_string(bufferFrames) + " frames, stream latency " +
                                std::to_string(latency / 10) + " us");
        check(client_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(capture_.out())),
              "Loopback capture client", RCS_ERROR_CAPTURE);
        check(client_->Start(), "Starting loopback capture", RCS_ERROR_CAPTURE);
    }

    void serve() {
        DeviceWatcher watcher;
        // Plain loopback delivers nothing while no sound plays. Silence stands
        // in for it then, so the stream never stops and the viewer's buffer
        // stays primed for the next sound.
        constexpr uint32_t silenceFrames = audioRate / 100;
        const auto silenceNs = uint64_t(silenceFrames) * 1'000'000'000 / audioRate;
        uint64_t covered = monotonicNs(); // Sound delivered up to here.
        while (!restarter_.stopping()) {
            // The event is a hint, so the timeout polls.
            WaitForSingleObject(event_, 10);
            if (const uint64_t now = monotonicNs(); now > covered + 2 * silenceNs) {
                samples_.assign(size_t(silenceFrames) * audioChannels, 0.0f);
                while (covered + silenceNs <= now - silenceNs) {
                    deliver_(samples_, covered);
                    covered += silenceNs;
                }
            }
            UINT32 frames = 0;
            check(capture_->GetNextPacketSize(&frames), "Capturing sound", RCS_ERROR_DEVICE);
            while (frames) {
                BYTE* data = nullptr;
                DWORD flags = 0;
                check(capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr), "Capturing sound",
                      RCS_ERROR_DEVICE);
                samples_.resize(size_t(frames) * audioChannels);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(samples_.begin(), samples_.end(), 0.0f);
                } else if (float_) {
                    std::memcpy(samples_.data(), data, samples_.size() * sizeof(float));
                } else {
                    const auto* pcm = reinterpret_cast<const int16_t*>(data);
                    for (size_t i = 0; i < samples_.size(); ++i) {
                        samples_[i] = float(pcm[i]) / 32768.0f;
                    }
                }
                check(capture_->ReleaseBuffer(frames), "Capturing sound", RCS_ERROR_DEVICE);
                period_ = frames;
                covered = monotonicNs();
                deliver_(samples_, covered - uint64_t(frames) * 1'000'000'000 / audioRate);
                check(capture_->GetNextPacketSize(&frames), "Capturing sound", RCS_ERROR_DEVICE);
            }
            if (!exclude_ && watcher.changed()) { // Follow the default device.
                const auto device = defaultOutput();
                if (!device || device->id != deviceId_) {
                    log::info("audio", "The default output device changed; capturing from the new one");
                    return;
                }
            }
        }
    }

    void close() {
        if (client_) {
            client_->Stop();
        }
        capture_.reset();
        client_.reset();
        if (event_) {
            CloseHandle(event_);
            event_ = nullptr;
        }
    }

    bool exclude_ = true, float_ = true;
    Deliver deliver_;
    Com<IAudioClient> client_;
    Com<IAudioCaptureClient> capture_;
    HANDLE event_ = nullptr;
    std::wstring deviceId_;
    std::vector<float> samples_;
    std::atomic<uint32_t> period_{0};
    Restarter restarter_;
};

// ---- Microphones ----------------------------------------------------------------------

// IAcousticEchoCancellationControl (Windows 11 22H2), missing from MinGW: a
// stream that offers it runs the device's echo cancellation.
constexpr GUID echoCancellationControl = {0xf4ae25b5, 0xaaa3, 0x437d, {0xb6, 0xb3, 0xdb, 0xbe, 0x2d, 0x0e, 0x95, 0x49}};

// A capture endpoint as a communications stream when voice processing is
// asked for: the device's own echo cancellation and noise suppression, where
// its driver has them (reported).
class WasapiMicrophone final : public AudioCapture {
public:
    explicit WasapiMicrophone(const MicrophoneConfig& config) : config_(config) {}
    ~WasapiMicrophone() override { stop(); }

    uint32_t periodFrames() const noexcept override { return period_.load(); }

    void start(const AudioCaptureConfig&, Deliver deliver, Status status) override {
        deliver_ = std::move(deliver);
        restarter_.start([this] { open(); }, [this] { serve(); }, [this] { close(); }, std::move(status),
                         [this] { return processing_.load(); });
    }
    void stop() noexcept override { restarter_.stop(); }

private:
    std::optional<Endpoint> endpoint() const {
        if (!config_.device) {
            return defaultEndpoint(eCapture, eCommunications);
        }
        for (auto& device : endpoints(eCapture)) {
            if (numberOf(device.id) == config_.device) {
                return std::move(device);
            }
        }
        return std::nullopt;
    }

    void open() {
        auto device = endpoint();
        if (!device) {
            fail(RCS_ERROR_DEVICE, config_.device ? "Sound device " + std::to_string(config_.device) +
                                                        " is no input that records now (rcs_list_audio_devices)"
                                                  : std::string("No microphone to record from"));
        }
        check(device->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(client_.out())),
              "Opening the microphone", RCS_ERROR_DEVICE);
        if (config_.voiceProcessing) {
            Com<IAudioClient2> communications;
            if (SUCCEEDED(client_->QueryInterface(__uuidof(IAudioClient2),
                                                  reinterpret_cast<void**>(communications.out())))) {
                AudioClientProperties properties{};
                properties.cbSize = sizeof(properties);
                properties.eCategory = AudioCategory_Communications;
                communications->SetClientProperties(&properties);
            }
        }
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const auto format = waveFormat(true);
        check(client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                  streamBuffer, 0, &format, nullptr),
              "Starting the microphone", RCS_ERROR_DEVICE);
        check(client_->SetEventHandle(event_), "Microphone events", RCS_ERROR_DEVICE);
        check(client_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(capture_.out())),
              "Microphone capture client", RCS_ERROR_DEVICE);
        processing_ = false;
        if (config_.voiceProcessing) {
            Com<IUnknown> control;
            processing_ = SUCCEEDED(client_->GetService(echoCancellationControl, reinterpret_cast<void**>(control.out())));
            if (!processing_) {
                log::info("audio", "The microphone " + device->name +
                                       " offers no echo cancellation here: it records without");
            }
        }
        deviceId_ = device->id;
        check(client_->Start(), "Starting the microphone", RCS_ERROR_DEVICE);
    }

    void serve() {
        DeviceWatcher watcher(eCapture, eCommunications);
        while (!restarter_.stopping()) {
            WaitForSingleObject(event_, 10); // A hint: the timeout polls.
            UINT32 frames = 0;
            check(capture_->GetNextPacketSize(&frames), "Recording", RCS_ERROR_DEVICE);
            while (frames) {
                BYTE* data = nullptr;
                DWORD flags = 0;
                check(capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr), "Recording", RCS_ERROR_DEVICE);
                samples_.resize(size_t(frames) * audioChannels);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(samples_.begin(), samples_.end(), 0.0f);
                } else {
                    std::memcpy(samples_.data(), data, samples_.size() * sizeof(float));
                }
                check(capture_->ReleaseBuffer(frames), "Recording", RCS_ERROR_DEVICE);
                period_ = frames;
                deliver_(samples_, monotonicNs() - uint64_t(frames) * 1'000'000'000 / audioRate);
                check(capture_->GetNextPacketSize(&frames), "Recording", RCS_ERROR_DEVICE);
            }
            if (!config_.device && watcher.changed()) { // Follow the default input.
                const auto device = defaultEndpoint(eCapture, eCommunications);
                if (!device || device->id != deviceId_) {
                    log::info("audio", "The default microphone changed; recording from the new one");
                    return;
                }
            }
        }
    }

    void close() {
        if (client_) {
            client_->Stop();
        }
        capture_.reset();
        client_.reset();
        if (event_) {
            CloseHandle(event_);
            event_ = nullptr;
        }
    }

    const MicrophoneConfig config_;
    Deliver deliver_;
    Com<IAudioClient> client_;
    Com<IAudioCaptureClient> capture_;
    HANDLE event_ = nullptr;
    std::wstring deviceId_;
    std::vector<float> samples_;
    std::atomic<uint32_t> period_{0};
    std::atomic<bool> processing_{false};
    Restarter restarter_;
};

// Whether the privacy settings let desktop programs use microphones: the
// device's, the user's and the one for programs that are not packaged.
bool microphoneAllowed() {
    const auto denied = [](HKEY root, const wchar_t* path) {
        wchar_t value[16] = {};
        DWORD size = sizeof(value);
        return RegGetValueW(root, path, L"Value", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS &&
               std::wstring(value) == L"Deny";
    };
    constexpr wchar_t store[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone";
    const std::wstring nonPackaged = std::wstring(store) + L"\\NonPackaged";
    return !denied(HKEY_LOCAL_MACHINE, store) && !denied(HKEY_CURRENT_USER, store) &&
           !denied(HKEY_CURRENT_USER, nonPackaged.c_str());
}

// ---- Playback -----------------------------------------------------------------------

class WasapiPlayer final : public AudioPlayer {
public:
    ~WasapiPlayer() override { stop(); }

    void start(const PlaybackConfig& config, Fill fill, Status status) override {
        config_ = config;
        fill_ = std::move(fill);
        restarter_.start([this] { open(); }, [this] { serve(); }, [this] { close(); },
                          [this, status = std::move(status)](const AudioStatus& s) {
                              if (s.active) {
                                  const auto d = device();
                                  log::info("audio", "Playing on " + d.name + ", " + std::to_string(d.periodFrames) +
                                                         " frames per period, " + std::to_string(d.queueFrames) +
                                                         " queued");
                              }
                              status(s);
                          });
    }
    void stop() noexcept override { restarter_.stop(); }

    Device device() const override {
        std::lock_guard lock(mutex_);
        Device device = device_;
        device.underruns = starved_.load(std::memory_order_relaxed);
        return device;
    }

private:
    void open() {
        auto device = defaultOutput();
        if (!device) {
            fail(RCS_ERROR_DEVICE, "No output device");
        }
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        openShared(*device);
        check(client_->SetEventHandle(event_), "Output device events", RCS_ERROR_DEVICE);
        check(client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(render_.out())),
              "Output render client", RCS_ERROR_DEVICE);
        check(client_->Start(), "Starting playback", RCS_ERROR_DEVICE);
        deviceId_ = device->id;
        std::lock_guard lock(mutex_);
        device_ = {device->name, period_, queue_};
    }

    // Shared mode: the engine's standard period, or a shorter one the host
    // asked for where the device offers it (IAudioClient3, a 48 kHz stereo
    // float mix). A period queued and a half by default: the one playing and
    // half of the next as margin for this thread waking late.
    void openShared(const Endpoint& device) {
        Com<IAudioClient3> low;
        WAVEFORMATEX* mix = nullptr;
        UINT32 standard = 0, fundamental = 0, minimum = 0, maximum = 0;
        bool floatMix = false;
        if (SUCCEEDED(device.device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr,
                                              reinterpret_cast<void**>(low.out()))) &&
            SUCCEEDED(low->GetMixFormat(&mix))) {
            // A short period needs the engine's periods: without them, the regular client's.
            floatMix = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                        (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                         reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->SubFormat == floatSubtype)) &&
                       mix->wBitsPerSample == 32 && mix->nSamplesPerSec == audioRate &&
                       mix->nChannels == audioChannels &&
                       SUCCEEDED(low->GetSharedModeEnginePeriod(mix, &standard, &fundamental, &minimum, &maximum));
        }
        const auto release = [&] {
            if (mix) {
                CoTaskMemFree(mix);
                mix = nullptr;
            }
        };
        const UINT32 wanted = config_.periodFrames;
        if (wanted && floatMix && wanted != standard) {
            const bool aligned = fundamental && wanted >= minimum && wanted <= maximum && (wanted - minimum) % fundamental == 0;
            if (!aligned) {
                const std::string range = std::to_string(minimum) + ".." + std::to_string(maximum) + " in steps of " +
                                          std::to_string(fundamental);
                release();
                fail(RCS_ERROR_UNSUPPORTED, "This device's shared mode takes periods of " + range + " frames");
            }
            const HRESULT opened = low->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, wanted, mix, nullptr);
            release();
            check(opened, "Opening the output device with a short period", RCS_ERROR_DEVICE);
            client_ = low.as<IAudioClient>("Output client", RCS_ERROR_DEVICE);
            period_ = wanted;
        } else {
            const std::string mixText = mix ? std::to_string(mix->nSamplesPerSec) + " Hz" : std::string("unknown");
            release();
            low.reset();
            check(device.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                          reinterpret_cast<void**>(client_.out())),
                  "Opening the output device", RCS_ERROR_DEVICE);
            const auto format = waveFormat(true);
            check(client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                          AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      0, 0, &format, nullptr),
                  "Opening the output device", RCS_ERROR_DEVICE);
            REFERENCE_TIME period = 0;
            check(client_->GetDevicePeriod(&period, nullptr), "The output device's period", RCS_ERROR_DEVICE);
            period_ = UINT32(period * audioRate / 10'000'000);
            if (wanted && wanted != period_) {
                fail(RCS_ERROR_UNSUPPORTED, "This device's shared mode runs at a fixed period of " +
                                                std::to_string(period_) + " frames (its mix is " + mixText +
                                                ")");
            }
        }
        check(client_->GetBufferSize(&bufferFrames_), "Output buffer", RCS_ERROR_DEVICE);
        queue_ = config_.queueFrames ? config_.queueFrames : period_ + period_ / 2;
        if (queue_ < period_ || queue_ > bufferFrames_) {
            fail(RCS_ERROR_UNSUPPORTED, "playback_queue_frames must be " + std::to_string(period_) + ".." +
                                            std::to_string(bufferFrames_) + " on this device");
        }
    }

    void serve() {
        DeviceWatcher watcher;
        while (!restarter_.stopping()) {
            WaitForSingleObject(event_, 50);
            UINT32 padding = 0;
            check(client_->GetCurrentPadding(&padding), "Playing sound", RCS_ERROR_DEVICE);
            const auto queued = Clock::now();
            // The device ran dry before this wake-up (its queue is empty), or
            // while this filled: the engine takes a period at each pass, the
            // next one a period after the pass that woke this thread.
            bool starved = started_ && padding == 0;
            if (const UINT32 frames = padding < queue_ ? queue_ - padding : 0) {
                BYTE* data = nullptr;
                check(render_->GetBuffer(frames, &data), "Playing sound", RCS_ERROR_DEVICE);
                fill_(reinterpret_cast<float*>(data), frames);
                check(render_->ReleaseBuffer(frames, 0), "Playing sound", RCS_ERROR_DEVICE);
                starved = starved || (started_ && Clock::now() - queued > std::chrono::microseconds(
                                                                               uint64_t(period_) * 1'000'000 / audioRate));
            }
            if (starved) {
                starved_.fetch_add(1, std::memory_order_relaxed);
            }
            started_ = true;
            if (watcher.changed()) { // Follow the default device.
                const auto device = defaultOutput();
                if (!device || device->id != deviceId_) {
                    log::info("audio", "The default output device changed");
                    return;
                }
            }
        }
    }

    void close() {
        if (client_) {
            client_->Stop();
        }
        if (started_) {
            log::debug("audio", "Output starved " + std::to_string(starved_.load()) + " times since start");
        }
        started_ = false;
        render_.reset();
        client_.reset();
        if (event_) {
            CloseHandle(event_);
            event_ = nullptr;
        }
        std::lock_guard lock(mutex_);
        device_ = {};
    }

    PlaybackConfig config_;
    Fill fill_;
    Com<IAudioClient> client_;
    Com<IAudioRenderClient> render_;
    HANDLE event_ = nullptr;
    UINT32 bufferFrames_ = 0, period_ = 0, queue_ = 0;
    bool started_ = false;
    std::atomic<uint64_t> starved_{0}; // Wake-ups that found the device's queue empty: audible gaps.
    std::wstring deviceId_;
    mutable std::mutex mutex_;
    Device device_;
    Restarter restarter_;
};

// ---- AAC (Media Foundation) ------------------------------------------------------------

// MFStartup is counted per process; COM is initialised on the creating thread
// and released there (a transform may be destroyed elsewhere: then the
// creating thread's COM stays until it ends).
class MfAudio {
protected:
    MfAudio() : thread_(GetCurrentThreadId()) {
        win::requireMediaFoundation();
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        uninitialize_ = SUCCEEDED(com);
        if (const HRESULT started = win::mfplat().startup(MF_VERSION, MFSTARTUP_LITE); FAILED(started)) {
            if (uninitialize_) { // The destructor does not run for an object that did not get made.
                CoUninitialize();
            }
            check(started, "Media Foundation startup", RCS_ERROR_CODEC);
        }
    }
    ~MfAudio() {
        transform_.reset();
        win::mfplat().shutdown();
        if (uninitialize_ && GetCurrentThreadId() == thread_) {
            CoUninitialize();
        }
    }

    Com<IMFMediaType> audioType(const GUID& subtype, uint32_t bits) {
        Com<IMFMediaType> type;
        check(win::mfplat().createMediaType(type.out()), "Media type", RCS_ERROR_CODEC);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        type->SetGUID(MF_MT_SUBTYPE, subtype);
        type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, audioRate);
        type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, audioChannels);
        type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bits);
        return type;
    }

    // Feeds `input`, `frames` of sound, and hands every output buffer to `output`.
    void process(const uint8_t* input, size_t size, uint32_t frames,
                 const std::function<void(const BYTE*, DWORD)>& output) {
        Com<IMFMediaBuffer> buffer;
        check(win::mfplat().createMemoryBuffer(DWORD(size), buffer.out()), "Audio buffer", RCS_ERROR_CODEC);
        BYTE* data = nullptr;
        buffer->Lock(&data, nullptr, nullptr);
        std::memcpy(data, input, size);
        buffer->Unlock();
        buffer->SetCurrentLength(DWORD(size));
        Com<IMFSample> sample;
        check(win::mfplat().createSample(sample.out()), "Audio sample", RCS_ERROR_CODEC);
        sample->AddBuffer(buffer.value);
        const LONGLONG duration = LONGLONG(frames) * 10'000'000 / audioRate; // 100 ns units.
        sample->SetSampleTime(position_);
        sample->SetSampleDuration(duration);
        position_ += duration;
        check(transform_->ProcessInput(0, sample.value, 0), "Audio codec input", RCS_ERROR_CODEC);
        MFT_OUTPUT_STREAM_INFO info{};
        check(transform_->GetOutputStreamInfo(0, &info), "Audio codec output", RCS_ERROR_CODEC);
        const bool provides = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
        for (;;) {
            Com<IMFSample> out;
            if (!provides) {
                Com<IMFMediaBuffer> outBuffer;
                check(win::mfplat().createMemoryBuffer(std::max<DWORD>(info.cbSize, 16384), outBuffer.out()),
                      "Audio buffer", RCS_ERROR_CODEC);
                check(win::mfplat().createSample(out.out()), "Audio sample", RCS_ERROR_CODEC);
                out->AddBuffer(outBuffer.value);
            }
            MFT_OUTPUT_DATA_BUFFER result{0, out.value, 0, nullptr};
            DWORD status = 0;
            const HRESULT hr = transform_->ProcessOutput(0, 1, &result, &status);
            if (result.pEvents) {
                result.pEvents->Release();
            }
            Com<IMFSample> produced;
            if (provides) {
                *produced.out() = result.pSample;
            } else {
                produced.assign(out.value);
            }
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                return;
            }
            check(hr, "Audio codec output", RCS_ERROR_CODEC);
            Com<IMFMediaBuffer> contiguous;
            check(produced->ConvertToContiguousBuffer(contiguous.out()), "Audio output", RCS_ERROR_CODEC);
            BYTE* bytes = nullptr;
            DWORD length = 0;
            contiguous->Lock(&bytes, nullptr, &length);
            output(bytes, length);
            contiguous->Unlock();
        }
    }

    Com<IMFTransform> transform_;

private:
    DWORD thread_;
    bool uninitialize_ = false;
    LONGLONG position_ = 0;
};

class AacEncoder final : public audio::Encoder, MfAudio {
public:
    explicit AacEncoder(uint32_t bitrate) {
        check(CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMFTransform),
                               reinterpret_cast<void**>(transform_.out())),
              "AAC encoder (Media Foundation)", RCS_ERROR_CODEC);
        auto output = audioType(MFAudioFormat_AAC, 16);
        output->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitrate / 8);
        output->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0); // Raw AAC, no ADTS.
        output->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        check(transform_->SetOutputType(0, output.value, 0), "AAC encoder output type", RCS_ERROR_CODEC);
        auto input = audioType(MFAudioFormat_PCM, 16);
        input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, audioChannels * 2);
        input->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, audioRate * audioChannels * 2);
        check(transform_->SetInputType(0, input.value, 0), "AAC encoder input type", RCS_ERROR_CODEC);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }

    uint32_t frames() const noexcept override { return aacFrames; }

    std::vector<Bytes> encode(std::span<const float> samples) override {
        pcm_.resize(samples.size());
        for (size_t i = 0; i < samples.size(); ++i) {
            pcm_[i] = int16_t(std::lrint(std::clamp(samples[i], -1.0f, 1.0f) * 32767.0f));
        }
        std::vector<Bytes> packets;
        process(reinterpret_cast<const uint8_t*>(pcm_.data()), pcm_.size() * sizeof(int16_t), aacFrames,
                [&](const BYTE* data, DWORD size) {
                    if (size > maxAudioPacketBytes) {
                        fail(RCS_ERROR_CODEC, "The AAC encoder made a packet of " + std::to_string(size) +
                                                  " bytes; one datagram takes " + std::to_string(maxAudioPacketBytes));
                    }
                    packets.emplace_back(data, data + size);
                });
        return packets;
    }

    Bytes config() const override { return {std::begin(aacConfig), std::end(aacConfig)}; }
    std::string name() const override { return "Microsoft AAC encoder"; }

private:
    std::vector<int16_t> pcm_;
};

class AacDecoder final : public audio::Decoder, MfAudio {
public:
    explicit AacDecoder(std::span<const uint8_t> config) {
        check(CoCreateInstance(aacDecoderClsid, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMFTransform),
                               reinterpret_cast<void**>(transform_.out())),
              "AAC decoder (Media Foundation)", RCS_ERROR_CODEC);
        auto input = audioType(MFAudioFormat_AAC, 16);
        input->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        input->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        // HEAACWAVEINFO after WAVEFORMATEX: payload type, profile, struct type,
        // reserved (12 bytes), then the AudioSpecificConfig.
        std::vector<uint8_t> user(12, 0);
        user[2] = 0x29;
        user.insert(user.end(), config.begin(), config.end());
        input->SetBlob(MF_MT_USER_DATA, user.data(), UINT32(user.size()));
        check(transform_->SetInputType(0, input.value, 0), "AAC decoder input type", RCS_ERROR_CODEC);
        // The decoder offers its output types once the input is set: float,
        // else 16-bit PCM, in stereo.
        Com<IMFMediaType> chosen;
        for (DWORD i = 0;; ++i) {
            Com<IMFMediaType> type;
            if (FAILED(transform_->GetOutputAvailableType(0, i, type.out()))) {
                break;
            }
            GUID subtype{};
            UINT32 channels = 0;
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
            if (channels != audioChannels || (subtype != MFAudioFormat_Float && subtype != MFAudioFormat_PCM)) {
                continue;
            }
            if (!chosen || subtype == MFAudioFormat_Float) {
                float_ = subtype == MFAudioFormat_Float;
                chosen = std::move(type);
            }
        }
        if (!chosen) {
            fail(RCS_ERROR_CODEC, "The AAC decoder offers no stereo PCM output");
        }
        check(transform_->SetOutputType(0, chosen.value, 0), "AAC decoder output type", RCS_ERROR_CODEC);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }

    void decode(std::span<const uint8_t> packet, std::vector<float>& out) override {
        try {
            process(packet.data(), packet.size(), aacFrames, [&](const BYTE* data, DWORD size) {
                if (float_) {
                    const auto* samples = reinterpret_cast<const float*>(data);
                    out.insert(out.end(), samples, samples + size / sizeof(float));
                } else {
                    const auto* samples = reinterpret_cast<const int16_t*>(data);
                    for (DWORD i = 0; i < size / sizeof(int16_t); ++i) {
                        out.push_back(float(samples[i]) / 32768.0f);
                    }
                }
            });
        } catch (const Failure& failure) {
            fail(RCS_ERROR_NETWORK, failure.what()); // A packet it cannot decode.
        }
    }

    std::string name() const override { return "Microsoft AAC decoder"; }

private:
    bool float_ = true;
};

} // namespace

std::unique_ptr<AudioCapture> createAudioCapture(rcs_audio_capture_backend) {
    return std::make_unique<WasapiLoopbackCapture>();
}

std::vector<AudioDevice> audioDevices() {
    ComThread com;
    std::vector<AudioDevice> result;
    for (const EDataFlow flow : {eCapture, eRender}) {
        const auto chosen = defaultEndpoint(flow, eConsole);
        for (const auto& device : endpoints(flow)) {
            result.push_back({numberOf(device.id), flow == eCapture, chosen && chosen->id == device.id, device.name});
        }
    }
    return result;
}

rcs_permission microphonePermission() noexcept {
    return microphoneAllowed() ? RCS_PERMISSION_GRANTED : RCS_PERMISSION_REQUIRED;
}

rcs_result requestMicrophonePermission() {
    return microphoneAllowed() ? RCS_OK : RCS_ERROR_PERMISSION_REQUIRED; // Only Settings changes it.
}

std::unique_ptr<AudioCapture> createMicrophoneCapture(const MicrophoneConfig& config) {
    return std::make_unique<WasapiMicrophone>(config);
}

uint32_t audioCaptureBackends() noexcept {
    return 1u << RCS_AUDIO_CAPTURE_WASAPI_LOOPBACK;
}

rcs_audio_capture_backend defaultAudioCaptureBackend() noexcept {
    return RCS_AUDIO_CAPTURE_WASAPI_LOOPBACK;
}

bool audioCapturePeriodSelectable(rcs_audio_capture_backend) noexcept {
    return false; // Loopback delivers the engine's periods.
}

bool audioExclusionAvailable() noexcept {
    return processLoopbackAvailable();
}

std::unique_ptr<AudioPlayer> createAudioPlayer() {
    return std::make_unique<WasapiPlayer>();
}

std::unique_ptr<audio::Encoder> createAacEncoder(uint32_t bitrate) {
    return std::make_unique<AacEncoder>(bitrate);
}

std::unique_ptr<audio::Decoder> createAacDecoder(std::span<const uint8_t> config) {
    return std::make_unique<AacDecoder>(config);
}

std::vector<uint32_t> aacBitrates() {
    return {96000, 128000, 160000, 192000}; // What the Media Foundation AAC encoder takes.
}

} // namespace rcsdk::platform
