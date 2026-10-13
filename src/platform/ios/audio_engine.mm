// The application's audio session and its one audio engine on iOS
// (rcs_audio_session_config). AVAudioSession is one per process, shared by
// the host and every session; so is the engine here. A thread of its own sets
// both up for every part that plays or records, and again whenever a part
// comes or goes, the settings change, or the system interrupts, reconfigures
// or resets them. Players are source nodes on the mixer, microphones share a
// tap on the input: iOS cancels the echo of what plays through the same
// voice processing I/O, so what the SDK plays is what a microphone's echo
// cancellation hears.
#include "platform/ios/audio_engine.hpp"

#include "media/audio.hpp"
#include "util/log.hpp"

#import <AVFoundation/AVFoundation.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace rcsdk::platform {
namespace ios {
namespace {

constexpr auto retryInterval = 2s;
constexpr uint32_t defaultPeriod = 256; // Frames at 48 kHz: 5.3 ms.
constexpr uint32_t maxFrames = 4096;    // A locked screen asks for up to 4096 frames at a time.

std::string text(NSError* error) {
    return error ? std::string(error.localizedDescription.UTF8String) : std::string("no reason given");
}

std::string categoryName(NSString* category) {
    return category ? std::string([category stringByReplacingOccurrencesOfString:@"AVAudioSessionCategory"
                                                                      withString:@""]
                                      .UTF8String)
                    : std::string("none");
}

bool sameStatus(const AudioStatus& a, const AudioStatus& b) {
    return a.active == b.active && a.result == b.result && a.message == b.message &&
           a.voiceProcessing == b.voiceProcessing;
}

// A player as the render block reaches it: cleared, and waited for, when it leaves.
struct PlayerSlot {
    explicit PlayerSlot(EnginePlayer* player) : target(player), scratch(size_t(maxFrames) * audioChannels) {}
    std::atomic<EnginePlayer*> target;
    std::atomic<int> rendering{0};
    std::vector<float> scratch; // Interleaved, before it goes to the node's two buffers.
};

// The real-time thread.
void render(PlayerSlot& slot, const AudioTimeStamp* time, AVAudioFrameCount frames, AudioBufferList* out,
            BOOL* silence) {
    slot.rendering.fetch_add(1);
    EnginePlayer* player = slot.target.load();
    auto* left = static_cast<float*>(out->mBuffers[0].mData);
    auto* right = out->mNumberBuffers > 1 ? static_cast<float*>(out->mBuffers[1].mData) : nullptr;
    if (!player || frames > maxFrames || !right) {
        for (UInt32 i = 0; i < out->mNumberBuffers; ++i) {
            std::memset(out->mBuffers[i].mData, 0, out->mBuffers[i].mDataByteSize);
        }
        *silence = YES;
    } else {
        float* samples = slot.scratch.data();
        player->render(samples, frames, time);
        for (AVAudioFrameCount i = 0; i < frames; ++i) {
            left[i] = samples[2 * i];
            right[i] = samples[2 * i + 1];
        }
    }
    slot.rendering.fetch_sub(1);
}

class Engine {
public:
    static Engine& instance() {
        static Engine* engine = new Engine; // Its thread and observers outlive any session.
        return *engine;
    }

    rcs_audio_session_config config() {
        std::lock_guard lock(mutex_);
        return config_;
    }
    // Applies at once to what plays and records. Once it returned, a setup
    // of the audio session in progress has ended, and none follows that
    // takes the earlier settings: a host that took the session over (HOST)
    // may set it up its way at once.
    void setConfig(const rcs_audio_session_config& config) {
        std::lock_guard session(session_);
        std::lock_guard lock(mutex_);
        config_ = config;
        changed();
    }

    void add(EnginePlayer* player) {
        std::lock_guard lock(mutex_);
        players_[player] = std::make_shared<PlayerSlot>(player);
        parts_.insert(player);
        changed();
    }
    void remove(EnginePlayer* player) {
        std::lock_guard calling(calling_);
        std::shared_ptr<PlayerSlot> slot;
        {
            std::lock_guard lock(mutex_);
            const auto found = players_.find(player);
            if (found == players_.end()) {
                return;
            }
            slot = found->second;
            players_.erase(found);
            parts_.erase(player);
            reported_.erase(player);
            changed();
        }
        slot->target = nullptr;
        while (slot->rendering.load()) {
            std::this_thread::yield();
        }
    }
    void add(EngineMicrophone* microphone) {
        std::lock_guard lock(mutex_);
        microphones_.push_back(microphone);
        parts_.insert(microphone);
        changed();
    }
    void remove(EngineMicrophone* microphone) {
        std::lock_guard calling(calling_);
        std::lock_guard delivering(delivering_);
        std::lock_guard lock(mutex_);
        std::erase(microphones_, microphone);
        parts_.erase(microphone);
        reported_.erase(microphone);
        changed();
    }

    uint32_t outputPeriod() {
        std::lock_guard lock(mutex_);
        return running_ ? applied_ : 0;
    }
    uint32_t inputPeriod() const noexcept { return inputPeriod_.load(); }

private:
    Engine() {
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        AVAudioSession* session = AVAudioSession.sharedInstance;
        [center addObserverForName:AVAudioSessionInterruptionNotification
                            object:session
                             queue:nil
                        usingBlock:^(NSNotification* note) {
                          const auto type = AVAudioSessionInterruptionType(
                              [note.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue]);
                          std::lock_guard lock(mutex_);
                          interrupted_ = type == AVAudioSessionInterruptionTypeBegan;
                          if (interrupted_) {
                              active_ = false; // The system deactivated it.
                          }
                          changed();
                        }];
        [center addObserverForName:AVAudioSessionMediaServicesWereResetNotification
                            object:session
                             queue:nil
                        usingBlock:^(NSNotification*) {
                          std::lock_guard lock(mutex_);
                          reset_ = true;
                          active_ = false;
                          changed();
                        }];
        std::thread([this] { run(); }).detach();
    }

    void changed() { // Holding mutex_.
        dirty_ = true;
        wake_.notify_all();
    }

    void run() noexcept {
        std::unique_lock lock(mutex_);
        for (;;) {
            if (!failing_) {
                wake_.wait(lock, [&] { return dirty_; });
            } else {
                wake_.wait_for(lock, retryInterval, [&] { return dirty_; }); // Again in a while.
            }
            dirty_ = false;
            lock.unlock();
            @autoreleasepool {
                try {
                    reconfigure();
                } catch (const std::exception& error) {
                    log::error("audio", std::string("The audio engine: ") + error.what());
                }
            }
            lock.lock();
        }
    }

    // ---- Setting up --------------------------------------------------------------------------

    // What the parts asked for, read while they were there: a part may leave
    // and be gone while the plan is set up, so its pointer only names it.
    struct Plan {
        struct Player {
            std::shared_ptr<PlayerSlot> slot;
            uint32_t period = 0;
        };
        struct Microphone {
            EngineMicrophone* part = nullptr;
            MicrophoneConfig config;
        };
        std::map<EnginePlayer*, Player> players;
        std::vector<Microphone> microphones;
        bool interrupted = false, reset = false;
    };

    void reconfigure() {
        Plan plan;
        {
            std::lock_guard lock(mutex_);
            for (const auto& [player, slot] : players_) {
                plan.players[player] = {slot, player->periodFrames()};
            }
            for (auto* microphone : microphones_) {
                plan.microphones.push_back({microphone, microphone->microphone()});
            }
            plan.interrupted = interrupted_;
            plan.reset = std::exchange(reset_, false);
            running_ = false;
        }
        tearDown(plan.reset);
        std::map<EnginePart*, AudioStatus> statuses;
        const auto all = [&](const AudioStatus& status) {
            for (const auto& [player, planned] : plan.players) {
                statuses[player] = status;
            }
            for (const auto& microphone : plan.microphones) {
                statuses[microphone.part] = status;
            }
        };
        bool failing = false;
        if (plan.players.empty() && plan.microphones.empty()) {
            deactivate();
        } else if (plan.interrupted) {
            all({false, RCS_ERROR_DEVICE, "The system interrupted sound (a call, Siri or another application)"});
        } else {
            try {
                build(plan, statuses);
            } catch (const Failure& failure) {
                tearDown(false);
                all({false, failure.result(), failure.what()});
                log::warning("audio", failure.what());
                failing = true;
            }
        }
        {
            std::lock_guard lock(mutex_);
            failing_ = failing;
        }
        report(statuses);
    }

    // Stops the engine and takes the parts off it; `discard`: the system's
    // media services were reset, and the engine with them.
    void tearDown(bool discard) {
        if (engine_) {
            if (tapped_) {
                [engine_.inputNode removeTapOnBus:0];
                tapped_ = false;
            }
            [engine_ stop];
            for (AVAudioSourceNode* node in nodes_) {
                [engine_ detachNode:node];
            }
        }
        nodes_ = [NSMutableArray array];
        inputPeriod_ = 0;
        if (discard && engine_) {
            [NSNotificationCenter.defaultCenter removeObserver:engineObserver_];
            engineObserver_ = nil;
            engine_ = nil;
        }
    }

    void build(const Plan& plan, std::map<EnginePart*, AudioStatus>& statuses) {
        // A microphone needs the user's permission; without it the others go on.
        std::vector<Plan::Microphone> microphones;
        for (const auto& microphone : plan.microphones) {
            if (AVAudioApplication.sharedInstance.recordPermission == AVAudioApplicationRecordPermissionGranted) {
                microphones.push_back(microphone);
            } else {
                statuses[microphone.part] = {false, RCS_ERROR_PERMISSION_REQUIRED,
                                        "This application may not record from microphones "
                                        "(rcs_request_microphone_permission)"};
            }
        }
        if (plan.players.empty() && microphones.empty()) {
            deactivate();
            return;
        }
        bool voiceProcessing = false;
        for (const auto& microphone : microphones) {
            voiceProcessing = voiceProcessing || microphone.config.voiceProcessing;
        }
        setUpSession(plan, microphones, voiceProcessing);
        const uint32_t applied = uint32_t(std::lround(AVAudioSession.sharedInstance.IOBufferDuration * audioRate));
        if (!engine_) {
            engine_ = [[AVAudioEngine alloc] init];
            // The system stopped it (the route or the hardware's format changed): set up again.
            engineObserver_ = [NSNotificationCenter.defaultCenter
                addObserverForName:AVAudioEngineConfigurationChangeNotification
                            object:engine_
                             queue:nil
                        usingBlock:^(NSNotification*) {
                          std::lock_guard lock(mutex_);
                          changed();
                        }];
        }
        AVAudioFormat* format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:audioRate
                                                                                channels:audioChannels];
        for (const auto& [player, planned] : plan.players) {
            // The session's period is a preference; a period a player asked for must be the one in use.
            if (const uint32_t asked = planned.period; asked && asked != applied) {
                statuses[player] = {false, RCS_ERROR_UNSUPPORTED,
                                    "The output runs at " + std::to_string(applied) + " frames per period, not the " +
                                        std::to_string(asked) + " asked for"};
                continue;
            }
            const std::shared_ptr<PlayerSlot> reached = planned.slot;
            AVAudioSourceNode* node = [[AVAudioSourceNode alloc]
                initWithFormat:format
                   renderBlock:^OSStatus(BOOL* silence, const AudioTimeStamp* time, AVAudioFrameCount frames,
                                         AudioBufferList* out) {
                     render(*reached, time, frames, out, silence);
                     return noErr;
                   }];
            [engine_ attachNode:node];
            [engine_ connect:node to:engine_.mainMixerNode format:format];
            [nodes_ addObject:node];
            statuses[player] = {true, RCS_OK, {}};
        }
        bool processing = false;
        if (!microphones.empty()) {
            AVAudioInputNode* input = engine_.inputNode;
            NSError* error = nil;
            processing = [input setVoiceProcessingEnabled:voiceProcessing error:&error] && voiceProcessing;
            if (voiceProcessing && !processing) {
                log::info("audio", "The microphone has no voice processing here (" + text(error) +
                                       "): it records without");
            }
            tap(input);
            for (const auto& microphone : microphones) {
                statuses[microphone.part] = {true, RCS_OK, {}, processing && microphone.config.voiceProcessing};
            }
        }
        [engine_ mainMixerNode]; // Output too for a microphone alone: voice processing runs on both.
        [engine_ prepare];
        NSError* error = nil;
        if (![engine_ startAndReturnError:&error]) {
            fail(RCS_ERROR_DEVICE, "The audio engine cannot start: " + text(error));
        }
        std::lock_guard lock(mutex_);
        running_ = true;
        applied_ = applied;
        log::info("audio", std::to_string(nodes_.count) + " player(s) and " + std::to_string(microphones.size()) +
                               " microphone(s) on " + outputNameNow() + ", " + std::to_string(applied) +
                               " frames per period" + (processing ? ", voice processing" : ""));
    }

    // The category for what plays and records, the period and the input
    // (RCS_AUDIO_SESSION_SDK); with HOST only whether the host's setup
    // carries it. By the settings as they are now, not as planned.
    void setUpSession(const Plan& plan, const std::vector<Plan::Microphone>& microphones, bool voiceProcessing) {
        std::lock_guard held(session_);
        AVAudioSession* session = AVAudioSession.sharedInstance;
        const bool record = !microphones.empty(), play = !plan.players.empty();
        const auto c = config();
        if (c.manager != RCS_AUDIO_SESSION_SDK) {
            NSString* category = session.category;
            const bool records = [category isEqualToString:AVAudioSessionCategoryPlayAndRecord] ||
                                 [category isEqualToString:AVAudioSessionCategoryRecord] ||
                                 [category isEqualToString:AVAudioSessionCategoryMultiRoute];
            if (record && !records) {
                fail(RCS_ERROR_DEVICE, "The host's audio session (category " + categoryName(category) +
                                           ", RCS_AUDIO_SESSION_HOST) does not record");
            }
            if (play && [category isEqualToString:AVAudioSessionCategoryRecord]) {
                fail(RCS_ERROR_DEVICE, "The host's audio session (category Record, RCS_AUDIO_SESSION_HOST) does not "
                                       "play sound");
            }
            return;
        }
        AVAudioSessionCategoryOptions options = c.mix_with_others ? AVAudioSessionCategoryOptionMixWithOthers : 0;
        if (record && c.default_to_speaker) {
            options |= AVAudioSessionCategoryOptionDefaultToSpeaker;
        }
        if (record && c.bluetooth_microphone) {
            options |= AVAudioSessionCategoryOptionAllowBluetoothHFP;
        }
        NSError* error = nil;
        if (![session setCategory:record ? AVAudioSessionCategoryPlayAndRecord : AVAudioSessionCategoryPlayback
                             mode:voiceProcessing ? AVAudioSessionModeVoiceChat : AVAudioSessionModeDefault
                          options:options
                            error:&error]) {
            fail(RCS_ERROR_DEVICE, "Setting up the audio session failed: " + text(error));
        }
        uint32_t period = 0; // The shortest any player asks for.
        for (const auto& [player, planned] : plan.players) {
            const uint32_t asked = planned.period ? planned.period : defaultPeriod;
            period = period ? std::min(period, asked) : asked;
        }
        // A preference: the system rounds it (what applies is reported).
        [session setPreferredIOBufferDuration:double(period ? period : defaultPeriod) / audioRate error:nil];
        if (![session setActive:YES error:&error]) {
            fail(RCS_ERROR_DEVICE, "Activating the audio session failed: " + text(error));
        }
        {
            std::lock_guard lock(mutex_);
            active_ = true;
        }
        if (record) { // The first microphone that names a device has it; 0 follows the system's choice.
            uint32_t device = 0;
            for (const auto& microphone : microphones) {
                device = device ? device : microphone.config.device;
            }
            AVAudioSessionPortDescription* chosen = nil;
            for (AVAudioSessionPortDescription* port in session.availableInputs) {
                if (device && deviceId(port.UID.UTF8String) == device) {
                    chosen = port;
                }
            }
            if (device && !chosen) {
                fail(RCS_ERROR_DEVICE, "Sound device " + std::to_string(device) +
                                           " is not an input now (rcs_list_audio_devices lists the inputs)");
            }
            if (![session setPreferredInput:chosen error:&error]) {
                fail(RCS_ERROR_DEVICE, "Choosing the microphone failed: " + text(error));
            }
        }
    }

    // The microphones' tap: the input as 48 kHz stereo, to each of them.
    void tap(AVAudioInputNode* input) {
        AVAudioFormat* native = [input outputFormatForBus:0];
        if (!native || native.sampleRate <= 0 || !native.channelCount) {
            fail(RCS_ERROR_DEVICE, "No microphone to record from");
        }
        AVAudioFormat* wanted = [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                                                 sampleRate:audioRate
                                                                   channels:audioChannels
                                                                interleaved:YES];
        AVAudioConverter* converter = [[AVAudioConverter alloc] initFromFormat:native toFormat:wanted];
        if (!converter) {
            fail(RCS_ERROR_DEVICE, "The microphone's format cannot be converted to 48 kHz stereo");
        }
        if (native.channelCount == 1) {
            converter.channelMap = @[ @0, @0 ];
        }
        const double ratio = audioRate / native.sampleRate;
        [input installTapOnBus:0
                    bufferSize:AVAudioFrameCount(native.sampleRate / 100) // 10 ms
                        format:native
                         block:^(AVAudioPCMBuffer* buffer, AVAudioTime*) {
                           deliver(converter, wanted, buffer, ratio);
                         }];
        tapped_ = true;
    }

    // The tap's thread.
    void deliver(AVAudioConverter* converter, AVAudioFormat* wanted, AVAudioPCMBuffer* buffer, double ratio) {
        const auto capacity = AVAudioFrameCount(buffer.frameLength * ratio) + 32;
        AVAudioPCMBuffer* out = [[AVAudioPCMBuffer alloc] initWithPCMFormat:wanted frameCapacity:capacity];
        __block bool given = false;
        NSError* error = nil;
        [converter convertToBuffer:out
                             error:&error
                withInputFromBlock:^AVAudioBuffer*(AVAudioPacketCount, AVAudioConverterInputStatus* status) {
                  if (given) {
                      *status = AVAudioConverterInputStatus_NoDataNow;
                      return nil;
                  }
                  given = true;
                  *status = AVAudioConverterInputStatus_HaveData;
                  return buffer;
                }];
        if (error || !out.frameLength) {
            return;
        }
        inputPeriod_ = out.frameLength;
        const uint64_t duration = uint64_t(out.frameLength) * 1'000'000'000 / audioRate;
        const std::span<const float> samples(out.floatChannelData[0], size_t(out.frameLength) * audioChannels);
        const uint64_t timestampNs = monotonicNs() - duration;
        std::lock_guard delivering(delivering_);
        std::vector<EngineMicrophone*> microphones;
        {
            std::lock_guard lock(mutex_);
            microphones = microphones_;
        }
        for (auto* microphone : microphones) {
            microphone->captured(samples, timestampNs);
        }
    }

    // After the last part left (RCS_AUDIO_SESSION_SDK): other applications may play again.
    void deactivate() {
        std::lock_guard held(session_);
        {
            std::lock_guard lock(mutex_);
            if (!active_ || config_.manager != RCS_AUDIO_SESSION_SDK) {
                return;
            }
            active_ = false;
        }
        NSError* error = nil;
        if (![AVAudioSession.sharedInstance setActive:NO
                                          withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                                                error:&error]) {
            log::warning("audio", "Deactivating the audio session failed: " + text(error));
        }
    }

    // Each part that is still there hears its status when it changed.
    void report(const std::map<EnginePart*, AudioStatus>& statuses) {
        std::lock_guard calling(calling_);
        for (const auto& [part, status] : statuses) {
            {
                std::lock_guard lock(mutex_);
                const auto last = reported_.find(part);
                if (!parts_.count(part) || (last != reported_.end() && sameStatus(last->second, status))) {
                    continue;
                }
                reported_[part] = status;
            }
            part->status(status);
        }
    }

    std::string outputNameNow() {
        NSArray<AVAudioSessionPortDescription*>* outputs = AVAudioSession.sharedInstance.currentRoute.outputs;
        return outputs.count ? std::string(outputs.firstObject.portName.UTF8String) : std::string("no output");
    }

    std::recursive_mutex calling_; // Held while parts hear their status, and to remove one.
    std::mutex session_;           // Held while the SDK sets the audio session up, and to change the settings.
    std::mutex delivering_;        // Held while the microphones get sound, and to remove one.
    std::mutex mutex_;             // Below.
    std::condition_variable wake_;
    rcs_audio_session_config config_ = RCS_AUDIO_SESSION_CONFIG_INIT;
    std::map<EnginePlayer*, std::shared_ptr<PlayerSlot>> players_;
    std::vector<EngineMicrophone*> microphones_;
    std::set<EnginePart*> parts_; // Both, as they joined and have not left.
    std::map<EnginePart*, AudioStatus> reported_;
    bool dirty_ = false, failing_ = false, interrupted_ = false, reset_ = false;
    bool active_ = false;  // The SDK activated the session and has not deactivated it.
    bool running_ = false; // The engine runs with the parts as last set up...
    uint32_t applied_ = 0; // ...at this period.
    std::atomic<uint32_t> inputPeriod_{0};
    // The engine's thread.
    AVAudioEngine* engine_ = nil;
    id engineObserver_ = nil;
    NSMutableArray<AVAudioSourceNode*>* nodes_ = [NSMutableArray array];
    bool tapped_ = false;
};

} // namespace

void add(EnginePlayer* player) {
    Engine::instance().add(player);
}
void remove(EnginePlayer* player) {
    Engine::instance().remove(player);
}
void add(EngineMicrophone* microphone) {
    Engine::instance().add(microphone);
}
void remove(EngineMicrophone* microphone) {
    Engine::instance().remove(microphone);
}

uint32_t outputPeriod() {
    return Engine::instance().outputPeriod();
}
uint32_t inputPeriod() {
    return Engine::instance().inputPeriod();
}

std::string outputName() {
    @autoreleasepool {
        NSArray<AVAudioSessionPortDescription*>* outputs = AVAudioSession.sharedInstance.currentRoute.outputs;
        return outputs.count ? std::string(outputs.firstObject.portName.UTF8String) : std::string();
    }
}

uint32_t deviceId(const std::string& uid) {
    uint32_t hash = 2166136261u; // FNV-1a
    for (const char c : uid) {
        hash = (hash ^ uint8_t(c)) * 16777619u;
    }
    return hash ? hash : 1;
}

} // namespace ios

void setAudioSessionConfig(const rcs_audio_session_config& config) {
    ios::Engine::instance().setConfig(config);
}

rcs_audio_session_config audioSessionConfig() {
    return ios::Engine::instance().config();
}

} // namespace rcsdk::platform
