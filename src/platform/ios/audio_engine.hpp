// The application's sound on iOS: one audio session and one audio engine for
// every session of the process (rcs_audio_session_config). Players are source
// nodes on its mixer and microphones a tap on its input, so that a
// microphone's echo cancellation hears what the SDK plays.
#pragma once

#include "platform/platform.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <span>
#include <string>

namespace rcsdk::platform::ios {

// A part of a session's sound on the engine. Its status calls never overlap
// and never come after its removal returned; they may come on the thread
// that removes another part. What it asks for (periodFrames, microphone) is
// read while it is added and stays as it was until it leaves.
class EnginePart {
public:
    virtual ~EnginePart() = default;
    virtual void status(const AudioStatus& status) = 0;
};

class EnginePlayer : public EnginePart {
public:
    // The real-time thread: `frames` frames of 48 kHz stereo interleaved
    // float32, for the render cycle stamped `time`.
    virtual void render(float* samples, uint32_t frames, const AudioTimeStamp* time) noexcept = 0;
    // The period it asks for, in frames at 48 kHz; 0: the SDK's own.
    virtual uint32_t periodFrames() const noexcept = 0;
};

class EngineMicrophone : public EnginePart {
public:
    // The input's thread: 48 kHz stereo interleaved float32.
    virtual void captured(std::span<const float> samples, uint64_t timestampNs) noexcept = 0;
    virtual const MicrophoneConfig& microphone() const noexcept = 0;
};

// Parts join and leave; the engine sets itself up again for what remains.
// remove() returns once no call into the part is in progress.
void add(EnginePlayer* player);
void remove(EnginePlayer* player);
void add(EngineMicrophone* microphone);
void remove(EngineMicrophone* microphone);

// While it runs: the period in frames at 48 kHz, the frames a microphone
// gets at a time, and the current output's name.
uint32_t outputPeriod();
uint32_t inputPeriod();
std::string outputName();

// A sound device's number, stable while it stays: from its port's UID, never 0.
uint32_t deviceId(const std::string& uid);

} // namespace rcsdk::platform::ios
