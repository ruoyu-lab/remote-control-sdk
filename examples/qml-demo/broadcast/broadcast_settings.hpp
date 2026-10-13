// iOS: what the demo's broadcast upload extension shares the whole screen
// with. The app writes it before the system starts the broadcast and again
// when the user changes it; the extension reads it when it starts and when
// told it changed. They share a keychain item (one keychain access group in
// both entitlements, which personal teams can use where they cannot use app
// groups) and a Darwin notification. The key is a secret: a keychain is where
// it belongs.
#pragma once

#include <rcsdk/rcsdk.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct BroadcastSettings {
    uint16_t port = 47990;
    std::string key; // 64 hex digits, from the pairing code.
    bool tcp = false;
    uint32_t longSide = 1920; // The stream's longer side at most; its shape follows the screen.
    uint32_t framesPerSecond = 60;
    uint32_t bitrateMbps = 20;
    std::vector<rcs_codec> codecs{RCS_CODEC_H264}; // Most preferred first.
    std::vector<rcs_audio_codec> audioCodecs{RCS_AUDIO_OPUS, RCS_AUDIO_AAC, RCS_AUDIO_PCM};
    bool sound = true; // What the device plays.
};

// Also tells a running extension (broadcastSettingsChanged).
bool saveBroadcastSettings(const BroadcastSettings& settings);
std::optional<BroadcastSettings> loadBroadcastSettings();
// The Darwin notification saving posts.
inline constexpr const char* broadcastSettingsChanged = "local.rcsdk.qmldemo.broadcast.settings";
