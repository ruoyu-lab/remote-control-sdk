// The machine-bound objects and facts the SDK uses - screen and audio capture,
// input injectors and permission, the desktop shown, Ctrl+Alt+Del, the
// application in the background - created here: the platform's, unless the SDK's own tests replaced
// them through the set* functions. Not part of the public interface.
#pragma once

#include "platform/platform.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace rcsdk::seams {

using CaptureFactory = std::function<std::unique_ptr<platform::ScreenCapture>(const platform::CaptureTarget& target)>;

// Sessions started afterwards share frames from `factory` instead of the screen
// (and skip screen-recording checks), one capture per stream it shares
// (`target`). Pass an empty function to restore.
void setCaptureFactory(CaptureFactory factory);
bool captureOverridden();
std::unique_ptr<platform::ScreenCapture> createScreenCapture(const platform::CaptureTarget& target);

// The machine lists `windows` instead of the system's windows; empty restores.
void setWindowList(std::function<std::vector<rcs_window_info>()> windows);
std::vector<rcs_window_info> windows();

// Thumbnails come from `capture` instead of the screen; empty restores.
using ThumbnailCapture = std::function<platform::Thumbnail(const platform::ThumbnailRequest& request)>;
void setThumbnailCapture(ThumbnailCapture capture);
platform::Thumbnail captureThumbnail(const platform::ThumbnailRequest& request);

// Sessions started afterwards capture sound from `factory` instead of the
// system. Pass an empty function to restore.
using AudioCaptureFactory = std::function<std::unique_ptr<platform::AudioCapture>()>;
void setAudioCaptureFactory(AudioCaptureFactory factory);
std::unique_ptr<platform::AudioCapture> createAudioCapture(rcs_audio_capture_backend backend);
bool audioCaptureOverridden();
// Microphones too, with their own factory (the test's microphone).
void setMicrophoneFactory(AudioCaptureFactory factory);
std::unique_ptr<platform::AudioCapture> createMicrophoneCapture(const platform::MicrophoneConfig& config);
// Sessions started afterwards play sound into `factory`'s players instead of
// the system's output (a test's device that pulls in real time, silently).
// Pass an empty function to restore.
using AudioPlayerFactory = std::function<std::unique_ptr<platform::AudioPlayer>()>;
void setAudioPlayerFactory(AudioPlayerFactory factory);
std::unique_ptr<platform::AudioPlayer> createAudioPlayer();

// Remote input without the operating system: sessions started afterwards
// inject through `factory`'s injectors (which see each event's stream),
// `permission` answers whether input is permitted, and `available` whether
// the system takes it now (without it, whether `permission` says GRANTED).
// Pass empty functions to restore.
// `types`, where given: the input types the machine executes
// (rcs_capabilities.input_types) instead of the system's, e.g. on a system
// that executes none (iOS).
struct InputOverride {
    std::function<std::unique_ptr<platform::InputInjector>(const platform::CaptureTarget& target,
                                                            const platform::InputOptions& options)>
        factory;
    std::function<rcs_permission()> permission;
    std::function<bool(bool secureDesktop)> available;
    std::optional<uint32_t> types;
};
void setInputOverride(InputOverride input);
std::unique_ptr<platform::InputInjector> createInputInjector(const platform::CaptureTarget& target,
                                                             const std::vector<uint32_t>& streamDisplays,
                                                             const platform::InputOptions& options);
rcs_permission inputPermission();
bool inputAvailable(bool secureDesktop);

// The machine has the networks `networks` lists instead of the system's
// (and says their kinds, costs and the user's wish to save data:
// network_facts), all of them the test's loopback: a socket that is to use
// one of them is bound to none. Empty restores.
void setNetworks(std::function<std::vector<platform::Network>()> networks);
std::vector<platform::Network> networks();
uint32_t networkFacts();
bool networksOverridden();

// The machine is connected to the Wi-Fi networks `wifi` lists instead of
// the system's (and says everything of them: wifi_facts). Empty restores.
void setWifi(std::function<std::vector<rcs_wifi_info>()> wifi);
std::vector<rcs_wifi_info> wifi();
uint32_t wifiFacts();

// The machine shows the desktop `state` says instead of the system's; empty restores.
void setDesktopState(std::function<platform::DesktopState()> state);
platform::DesktopState desktopState();

// Ctrl+Alt+Del goes to `send` instead of the system, which makes the machine
// able to send it (rcs_capabilities.secure_attention); empty restores.
void setSecureAttention(std::function<rcs_result()> send);
// To the console session (platform::secureAttention).
rcs_result secureAttention();

// Whether this process counts as able to follow secure desktops and to send
// Ctrl+Alt+Del, regardless of the system; no value restores the system's answer.
struct SecureCapabilities {
    std::optional<bool> desktop, attention;
};
void setSecureCapabilities(SecureCapabilities capabilities);
// The platform's answers in `capabilities` become what the session is told:
// the secure ones above, and the input types of an InputOverride.
void applyCapabilities(rcs_capabilities& capabilities);

// The application moving to the background and back (iOS, through
// platform::observeAppState): `changed(background)` at each change from now
// on, until the watch returned is destroyed (which waits for a call of it in
// progress); appInBackground() is the state now. Calls of every watch come
// one at a time, on the system's thread (or the test's).
class AppStateWatch {
public:
    virtual ~AppStateWatch() = default;
};
std::unique_ptr<AppStateWatch> watchAppState(std::function<void(bool background)> changed);
bool appInBackground();
// The application is in the background (true) or in front (false) as far as
// sessions can tell, whatever the system says; no value restores the system's.
void setAppBackground(std::optional<bool> background);

} // namespace rcsdk::seams
