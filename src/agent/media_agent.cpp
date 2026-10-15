#include "agent/media_agent.hpp"

#include "seams/seams.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <tuple>

namespace rcsdk::agent {

MediaAgent::MediaAgent(Events& events)
    : events_(events),
      modules_(RCS_ROLE_SHARE,
               {[this](const std::string& id, rcs_result result, const std::string& message) {
                    events_.moduleStatus(id, result, message);
                },
                [this](const std::string& id, Bytes bytes) { events_.moduleData(id, std::move(bytes)); },
                [this] {
                    std::lock_guard lock(inputMutex_);
                    return shared_;
                }}) {
    watcher_ = log::thread([this] { watch(); });
}

MediaAgent::~MediaAgent() {
    {
        std::lock_guard lock(watchMutex_);
        quit_ = true;
    }
    watchWake_.notify_all();
    watcher_.join();
    stopAudio();
    std::array<std::unique_ptr<VideoPipeline>, RCS_MAX_STREAMS> video;
    {
        std::lock_guard lock(videoMutex_);
        video = std::move(video_);
    }
    // Destroyed unlocked (a last event may call the Port): no video event comes once this returns.
}

void MediaAgent::setHostEncoder(const rcs_host_encoder_config& config) {
    hostEncoder_ = config;
    for (const rcs_codec codec : config.codecs) {
        hostEncoders_ |= codec ? 1u << codec : 0;
    }
    hostEncoderFormats_ = config.picture_formats;
}

Machine MediaAgent::machine() const {
    Machine machine = localMachine();
    machine.hostEncoders = hostEncoders_;
    machine.hostEncoderFormats = hostEncoderFormats_;
    return machine;
}

// ---- Video -------------------------------------------------------------------------------------

VideoPipeline* MediaAgent::pipeline(uint32_t index) const noexcept {
    return index < video_.size() ? video_[index].get() : nullptr;
}

void MediaAgent::startVideo(const VideoStart& start) {
    if (start.index >= video_.size() || start.encoding >= RCS_MAX_ENCODINGS) {
        fail(RCS_ERROR_INTERNAL, "A stream beyond RCS_MAX_STREAMS, or an encoding beyond RCS_MAX_ENCODINGS");
    }
    std::lock_guard lock(videoMutex_);
    auto& video = video_[start.index];
    if (!video) {
        video = std::make_unique<VideoPipeline>(events_, start.index);
        video->setHostCapture(hostCapture_);
        video->setPreview(preview_);
        if (hostEncoder_) {
            video->setHostEncoder(*hostEncoder_);
        }
        video->setBacklogSource(backlog_);
        video->setCursorInVideo(cursorInVideo_);
        video->setInputBoost(boostMs_);
    }
    video->start(start);
}

void MediaAgent::stopVideo(uint32_t index, uint32_t encoding) {
    std::lock_guard lock(videoMutex_);
    if (auto* video = pipeline(index)) {
        video->stop(encoding);
    }
}

void MediaAgent::setRate(uint32_t index, uint32_t encoding, uint32_t bitrate, bool budgeted) {
    std::lock_guard lock(videoMutex_);
    if (auto* video = pipeline(index)) {
        video->setRate(encoding, bitrate, budgeted);
    }
}

void MediaAgent::setCursorInVideo(bool inVideo) {
    std::lock_guard lock(videoMutex_);
    cursorInVideo_ = inVideo;
    for (const auto& video : video_) {
        if (video) {
            video->setCursorInVideo(inVideo);
        }
    }
}

void MediaAgent::setInputBoost(uint32_t ms) {
    std::lock_guard lock(videoMutex_);
    boostMs_ = ms;
    for (const auto& video : video_) {
        if (video) {
            video->setInputBoost(ms);
        }
    }
}

void MediaAgent::requestKeyframe(uint32_t index, uint32_t encoding) {
    std::lock_guard lock(videoMutex_);
    if (auto* video = pipeline(index)) {
        video->requestKeyframe(encoding);
    }
}

void MediaAgent::invalidateAfter(uint32_t index, uint32_t encoding, uint64_t sequence) {
    std::lock_guard lock(videoMutex_);
    if (auto* video = pipeline(index)) {
        video->invalidateAfter(encoding, sequence);
    }
}

void MediaAgent::setBacklogSource(std::function<uint64_t(uint32_t)> backlogNs) {
    std::lock_guard lock(videoMutex_);
    backlog_ = backlogNs;
    for (const auto& video : video_) {
        if (video) {
            video->setBacklogSource(backlog_);
        }
    }
}

// ---- Sound -------------------------------------------------------------------------------------

void MediaAgent::startAudio(const AudioStart& start) {
    std::lock_guard lock(audioMutex_);
    if (audio_) {
        audio_->stop();
        audio_.reset();
    }
    try {
        audio_ = start.microphone ? seams::createMicrophoneCapture({start.device, start.voiceProcessing})
                                  : seams::createAudioCapture(start.backend);
        auto* capture = audio_.get();
        audio_->start({start.excludeCurrentProcess, start.periodFrames},
                      log::bound([this, capture](std::span<const float> samples, uint64_t timestampNs) {
                          events_.audioPcm(samples, timestampNs, capture->periodFrames());
                      }),
                      log::bound([this](const platform::AudioStatus& status) { events_.audioStatus(status); }));
    } catch (const Failure& failure) {
        audio_.reset();
        events_.audioStatus({false, failure.result(), failure.what()});
    }
}

void MediaAgent::stopAudio() {
    std::lock_guard lock(audioMutex_);
    if (audio_) {
        audio_->stop(); // No sound is delivered once this returns.
        audio_.reset();
    }
}

// ---- Input -------------------------------------------------------------------------------------

void MediaAgent::configureInput(const platform::CaptureTarget& target, const std::vector<uint32_t>& streamDisplays,
                                const platform::InputOptions& options) {
    // What modules are told is shared (rcs_module_get_shared): the main display
    // (display id 0) named as it is now, as input resolves it when it starts.
    auto shared = target;
    if (shared.source != RCS_CAPTURE_WINDOW && !shared.displayId) {
        const auto displays = platform::displays();
        const auto main = std::find_if(displays.begin(), displays.end(),
                                       [](const rcs_display_info& d) { return d.is_main != 0; });
        shared.displayId = main != displays.end() ? main->display_id : 0;
    }
    std::unique_ptr<platform::InputInjector> injector;
    try {
        injector = seams::createInputInjector(target, streamDisplays, options);
    } catch (const Failure& failure) {
        // No display to inject into (a machine without one, as a server
        // sharing the host's own pictures): input is unavailable, as on a lock
        // screen, and the picture goes on; a grant waits for the next viewer.
        log::warning("input", std::string("Remote input is unavailable: ") + failure.what());
    }
    const bool none = !injector;
    {
        std::lock_guard lock(inputMutex_);
        if (injector_) {
            injector_->releaseAll();
        }
        injector_ = std::move(injector);
        shared_ = shared;
        secureInput_ = options.secureDesktop;
        available_ = none ? 0 : -1;
    }
    if (none) {
        std::lock_guard report(reportMutex_);
        events_.inputAvailability(false);
        return;
    }
    checkInput(); // Reported at once, not at the watcher's next round.
}

rcs_result MediaAgent::applyInput(const rcs_input_event& event) {
    rcs_result result;
    {
        std::lock_guard lock(inputMutex_);
        if (!injector_) {
            return RCS_ERROR_INVALID_STATE;
        }
        result = injector_->apply(event);
        if (result == RCS_ERROR_PERMISSION_REQUIRED) {
            // The OS refused this event (an elevated window, or the desktop
            // switched before the next availability check). Release what is
            // held; the grant follows the OS's availability.
            injector_->releaseAll();
        }
    }
    if (result == RCS_OK) {
        std::lock_guard lock(videoMutex_);
        for (const auto& video : video_) {
            if (video) {
                video->boost();
            }
        }
    }
    return result;
}

void MediaAgent::releaseInput() {
    std::lock_guard lock(inputMutex_);
    if (injector_) {
        injector_->releaseAll();
    }
}

// ---- The watcher: input availability and the display list --------------------------------------

void MediaAgent::watch() {
    std::unique_lock lock(watchMutex_);
    while (!quit_) {
        watchWake_.wait_for(lock, 250ms, [&] { return quit_; });
        if (quit_) {
            break;
        }
        lock.unlock();
        try {
            const auto now = Clock::now();
            if (now >= nextInputCheck_) {
                nextInputCheck_ = now + 1s;
                checkInput();
            }
            if (now >= nextDisplayCheck_) {
                nextDisplayCheck_ = now + 2s;
                checkDisplays();
            }
            checkDesktop(); // Every round: a lock screen should be known as it comes.
        } catch (const std::exception& error) { // The next round tries again.
            watchFailed(error.what());
        }
        lock.lock();
    }
}

// What the watcher could not find out, in the log: once, and again only when
// it changes or a minute has passed.
void MediaAgent::watchFailed(const std::string& problem) {
    const auto now = Clock::now();
    if (problem == watchProblem_ && now < nextWatchReport_) {
        return;
    }
    watchProblem_ = problem;
    nextWatchReport_ = now + 60s;
    log::warning("agent", "Watching the machine failed (the next round tries again): " + problem);
}

// Whether the OS accepts injected input: a lock screen, a UAC prompt on the
// secure desktop or withdrawn permission does not. Held keys are released when
// it stops.
void MediaAgent::checkInput() {
    std::lock_guard report(reportMutex_);
    std::unique_lock lock(inputMutex_);
    if (!injector_) {
        return;
    }
    const bool available = seams::inputAvailable(secureInput_);
    if (int(available) == available_) {
        return;
    }
    available_ = int(available);
    if (!available) {
        injector_->releaseAll();
    }
    lock.unlock();
    events_.inputAvailability(available);
}

std::vector<rcs_window_info> MediaAgent::windows() {
    return seams::windows();
}

// A display connected, disconnected, changed its mode or was turned (also
// upside down, which leaves its size as it was).
void MediaAgent::checkDisplays() {
    if (seams::captureOverridden()) {
        return;
    }
    const auto list = platform::displays();
    const auto same = [](const rcs_display_info& a, const rcs_display_info& b) {
        return std::tie(a.display_id, a.pixel_width, a.pixel_height, a.refresh_hz, a.rotation_degrees, a.is_main) ==
               std::tie(b.display_id, b.pixel_width, b.pixel_height, b.refresh_hz, b.rotation_degrees, b.is_main);
    };
    const bool changed =
        displaysKnown_ && !std::equal(list.begin(), list.end(), displays_.begin(), displays_.end(), same);
    displays_ = list;
    displaysKnown_ = true;
    if (changed) {
        events_.displays(list);
    }
}

// What the machine shows: the user's desktop or a secure one.
void MediaAgent::checkDesktop() {
    const auto state = seams::desktopState();
    if (desktop_ && *desktop_ == state) {
        return;
    }
    desktop_ = state;
    events_.desktopState(state);
}

} // namespace rcsdk::agent
