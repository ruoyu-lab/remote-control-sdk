#include "seams/seams.hpp"

#include <mutex>
#include <utility>
#include <vector>

namespace rcsdk::seams {
namespace {
std::mutex mutex;
CaptureFactory captureOverride;
std::function<std::vector<rcs_window_info>()> windowsOverride;
std::function<std::vector<platform::Network>()> networksOverride;
std::function<std::vector<rcs_wifi_info>()> wifiOverride;
ThumbnailCapture thumbnailOverride;
AudioCaptureFactory audioOverride, microphoneOverride;
AudioPlayerFactory playerOverride;
InputOverride inputOverride;
std::function<platform::DesktopState()> desktopOverride;
std::function<rcs_result()> secureAttentionOverride;
SecureCapabilities secureCapabilities;
} // namespace

void setCaptureFactory(CaptureFactory factory) {
    std::lock_guard lock(mutex);
    captureOverride = std::move(factory);
}

bool captureOverridden() {
    std::lock_guard lock(mutex);
    return bool(captureOverride);
}

std::unique_ptr<platform::ScreenCapture> createScreenCapture(const platform::CaptureTarget& target) {
    std::lock_guard lock(mutex);
    return captureOverride ? captureOverride(target) : platform::createScreenCapture(target);
}

void setWindowList(std::function<std::vector<rcs_window_info>()> windows) {
    std::lock_guard lock(mutex);
    windowsOverride = std::move(windows);
}

std::vector<rcs_window_info> windows() {
    std::unique_lock lock(mutex);
    const auto list = windowsOverride;
    lock.unlock();
    return list ? list() : platform::windows();
}

void setNetworks(std::function<std::vector<platform::Network>()> networks) {
    std::lock_guard lock(mutex);
    networksOverride = std::move(networks);
}

std::vector<platform::Network> networks() {
    std::unique_lock lock(mutex);
    const auto list = networksOverride;
    lock.unlock();
    return list ? list() : platform::networks();
}

uint32_t networkFacts() {
    return networksOverridden() ? RCS_NETWORK_FACT_KIND | RCS_NETWORK_FACT_METERED | RCS_NETWORK_FACT_CONSTRAINED
                                : platform::networkFacts();
}

bool networksOverridden() {
    std::lock_guard lock(mutex);
    return bool(networksOverride);
}

void setWifi(std::function<std::vector<rcs_wifi_info>()> wifi) {
    std::lock_guard lock(mutex);
    wifiOverride = std::move(wifi);
}

std::vector<rcs_wifi_info> wifi() {
    std::unique_lock lock(mutex);
    const auto list = wifiOverride;
    lock.unlock();
    return list ? list() : platform::wifiNetworks();
}

uint32_t wifiFacts() {
    std::unique_lock lock(mutex);
    const bool overridden = bool(wifiOverride);
    lock.unlock();
    return overridden ? RCS_WIFI_FACT_CHANNEL | RCS_WIFI_FACT_WIDTH | RCS_WIFI_FACT_SIGNAL | RCS_WIFI_FACT_NOISE |
                            RCS_WIFI_FACT_RATE | RCS_WIFI_FACT_PEER_TO_PEER
                      : platform::wifiFacts();
}

void setThumbnailCapture(ThumbnailCapture capture) {
    std::lock_guard lock(mutex);
    thumbnailOverride = std::move(capture);
}

platform::Thumbnail captureThumbnail(const platform::ThumbnailRequest& request) {
    std::unique_lock lock(mutex);
    const auto capture = thumbnailOverride;
    lock.unlock();
    return capture ? capture(request) : platform::captureThumbnail(request);
}

bool audioCaptureOverridden() {
    std::lock_guard lock(mutex);
    return bool(audioOverride);
}

void setAudioCaptureFactory(AudioCaptureFactory factory) {
    std::lock_guard lock(mutex);
    audioOverride = std::move(factory);
}

std::unique_ptr<platform::AudioCapture> createAudioCapture(rcs_audio_capture_backend backend) {
    std::lock_guard lock(mutex);
    return audioOverride ? audioOverride() : platform::createAudioCapture(backend);
}

void setMicrophoneFactory(AudioCaptureFactory factory) {
    std::lock_guard lock(mutex);
    microphoneOverride = std::move(factory);
}

std::unique_ptr<platform::AudioCapture> createMicrophoneCapture(const platform::MicrophoneConfig& config) {
    std::lock_guard lock(mutex);
    return microphoneOverride ? microphoneOverride() : platform::createMicrophoneCapture(config);
}

void setAudioPlayerFactory(AudioPlayerFactory factory) {
    std::lock_guard lock(mutex);
    playerOverride = std::move(factory);
}

std::unique_ptr<platform::AudioPlayer> createAudioPlayer() {
    std::lock_guard lock(mutex);
    return playerOverride ? playerOverride() : platform::createAudioPlayer();
}

void setInputOverride(InputOverride input) {
    std::lock_guard lock(mutex);
    inputOverride = std::move(input);
}

std::unique_ptr<platform::InputInjector> createInputInjector(const platform::CaptureTarget& target,
                                                             const std::vector<uint32_t>& streamDisplays,
                                                             const platform::InputOptions& options) {
    std::unique_lock lock(mutex);
    const auto factory = inputOverride.factory;
    lock.unlock();
    return factory ? factory(target, options)
                   : platform::createInputInjector(target, streamDisplays, options);
}

rcs_permission inputPermission() {
    std::unique_lock lock(mutex);
    const auto permission = inputOverride.permission;
    lock.unlock();
    return permission ? permission() : platform::inputPermission();
}

bool inputAvailable(bool secureDesktop) {
    std::unique_lock lock(mutex);
    const auto available = inputOverride.available;
    const auto permission = inputOverride.permission;
    lock.unlock();
    if (available) {
        return available(secureDesktop);
    }
    return permission ? permission() == RCS_PERMISSION_GRANTED : platform::inputAvailable(secureDesktop);
}

void setDesktopState(std::function<platform::DesktopState()> state) {
    std::lock_guard lock(mutex);
    desktopOverride = std::move(state);
}

platform::DesktopState desktopState() {
    std::unique_lock lock(mutex);
    const auto state = desktopOverride;
    lock.unlock();
    return state ? state() : platform::desktopState();
}

void setSecureAttention(std::function<rcs_result()> send) {
    std::lock_guard lock(mutex);
    secureAttentionOverride = std::move(send);
}

rcs_result secureAttention(uint32_t forProcess) {
    std::unique_lock lock(mutex);
    const auto send = secureAttentionOverride;
    lock.unlock();
    return send ? send() : platform::secureAttention(forProcess);
}

void setSecureCapabilities(SecureCapabilities capabilities) {
    std::lock_guard lock(mutex);
    secureCapabilities = capabilities;
}

void applyCapabilities(rcs_capabilities& capabilities) {
    std::lock_guard lock(mutex);
    if (inputOverride.types) {
        capabilities.input_types = *inputOverride.types;
    }
    if (secureCapabilities.desktop) {
        capabilities.secure_desktop = *secureCapabilities.desktop ? 1 : 0;
    }
    if (secureCapabilities.attention) {
        capabilities.secure_attention = *secureCapabilities.attention ? 1 : 0;
    } else if (secureAttentionOverride) {
        capabilities.secure_attention = 1;
    }
    constexpr uint32_t attention = 1u << RCS_INPUT_SECURE_ATTENTION;
    capabilities.input_types =
        capabilities.secure_attention ? capabilities.input_types | attention : capabilities.input_types & ~attention;
}

// ---- The application's state -------------------------------------------------

namespace {

struct AppWatchEntry {
    std::mutex calling; // Held while `changed` runs, and to end the watch.
    std::function<void(bool)> changed;
};

class AppWatchHandle final : public AppStateWatch {
public:
    explicit AppWatchHandle(std::shared_ptr<AppWatchEntry> entry) : entry_(std::move(entry)) {}
    ~AppWatchHandle() override {
        std::lock_guard lock(entry_->calling);
        entry_->changed = nullptr;
    }

private:
    std::shared_ptr<AppWatchEntry> entry_;
};

std::mutex appTelling; // One change told at a time, in order.
std::mutex appMutex;   // Below.
bool appObserved = false;
bool appSystem = false; // In the background, as the system last said.
std::optional<bool> appOverride;
std::vector<std::weak_ptr<AppWatchEntry>> appWatches;

bool appStateLocked() {
    return appOverride.value_or(appSystem);
}

// Applies `change` to the state and tells the watches when it moved.
void changeAppState(const std::function<void()>& change) {
    std::lock_guard telling(appTelling);
    std::vector<std::shared_ptr<AppWatchEntry>> watches;
    bool now;
    {
        std::lock_guard lock(appMutex);
        const bool before = appStateLocked();
        change();
        now = appStateLocked();
        if (now == before) {
            return;
        }
        for (const auto& weak : appWatches) {
            if (auto watch = weak.lock()) {
                watches.push_back(std::move(watch));
            }
        }
    }
    for (const auto& watch : watches) {
        std::lock_guard calling(watch->calling);
        if (watch->changed) {
            watch->changed(now);
        }
    }
}

} // namespace

std::unique_ptr<AppStateWatch> watchAppState(std::function<void(bool background)> changed) {
    auto entry = std::make_shared<AppWatchEntry>();
    entry->changed = std::move(changed);
    bool observe;
    {
        std::lock_guard lock(appMutex);
        std::erase_if(appWatches, [](const std::weak_ptr<AppWatchEntry>& weak) { return weak.expired(); });
        appWatches.push_back(entry);
        observe = !std::exchange(appObserved, true);
    }
    if (observe) {
        platform::observeAppState([](bool background) { changeAppState([background] { appSystem = background; }); });
    }
    return std::make_unique<AppWatchHandle>(std::move(entry));
}

bool appInBackground() {
    std::lock_guard lock(appMutex);
    return appStateLocked();
}

void setAppBackground(std::optional<bool> background) {
    changeAppState([background] { appOverride = background; });
}

} // namespace rcsdk::seams
