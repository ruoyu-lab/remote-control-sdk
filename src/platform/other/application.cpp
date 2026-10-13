// The application's state and audio session on systems that have neither
// (macOS, Windows and those without a native backend): an application there
// may use the GPU whether it is in front or not, and its sound needs no
// session set up. (iOS: platform/ios/app_state.mm and audio_session.mm.)
#include "platform/platform.hpp"

namespace rcsdk::platform {

void setAudioSessionConfig(const rcs_audio_session_config&) {
    fail(RCS_ERROR_UNSUPPORTED, "Only iOS has an audio session to set up (rcs_audio_session_config)");
}

rcs_audio_session_config audioSessionConfig() {
    fail(RCS_ERROR_UNSUPPORTED, "Only iOS has an audio session to set up (rcs_audio_session_config)");
}

void observeAppState(std::function<void(bool)>) {}

} // namespace rcsdk::platform
