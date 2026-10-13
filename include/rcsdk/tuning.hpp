// Remote Control SDK - C++ wrapper of the tuning layer (rcsdk/tuning.h).
//
// A tuning extension the host chains is its own entirely: start from the
// session's preset, e.g.
//   config.extension<rcs_video_tuning_config>().input_boost_ms = 100;
// takes the preset's other fields (SessionConfig::extension), and
//   auto video = rcsdk::defaults<rcs_video_tuning_config>(RCS_PRESET_LOW_LATENCY);
// a preset's for Session::update.
#pragma once

#include <rcsdk/rcsdk.hpp>
#include <rcsdk/tuning.h>

namespace rcsdk {

template <>
inline rcs_video_tuning_config defaults<rcs_video_tuning_config>() noexcept {
    return RCS_VIDEO_TUNING_CONFIG_INIT;
}
template <>
inline rcs_audio_tuning_config defaults<rcs_audio_tuning_config>() noexcept {
    return RCS_AUDIO_TUNING_CONFIG_INIT;
}
template <>
inline rcs_network_tuning_config defaults<rcs_network_tuning_config>() noexcept {
    return RCS_NETWORK_TUNING_CONFIG_INIT;
}

} // namespace rcsdk
