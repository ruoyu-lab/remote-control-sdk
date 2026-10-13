// Sound formats of Core Audio, as the SDK uses them on macOS and iOS.
#pragma once

#include "media/audio.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <string>

namespace rcsdk::platform::apple {

// 48 kHz stereo interleaved float32, the SDK's sound everywhere.
inline AudioStreamBasicDescription floatFormat() {
    AudioStreamBasicDescription f{};
    f.mSampleRate = audioRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = audioChannels;
    f.mBitsPerChannel = 32;
    f.mBytesPerFrame = f.mBytesPerPacket = audioChannels * 4;
    f.mFramesPerPacket = 1;
    return f;
}

inline AudioStreamBasicDescription aacFormat() {
    AudioStreamBasicDescription f{};
    f.mSampleRate = audioRate;
    f.mFormatID = kAudioFormatMPEG4AAC;
    f.mFormatFlags = kMPEG4Object_AAC_LC;
    f.mChannelsPerFrame = audioChannels;
    f.mFramesPerPacket = aacFrames;
    return f;
}

inline std::string osStatusText(OSStatus status) {
    return std::to_string(int(status));
}

} // namespace rcsdk::platform::apple
