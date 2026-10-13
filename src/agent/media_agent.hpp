// The agent itself (agent.hpp): a pipeline per picture (stream), the sound
// capture, the input injector, a watcher of the displays and of whether input
// is accepted, and the modules' machine's halves, on the machine this object
// runs on. The session holds one in its own process (embedded), or a host
// runs one in a process of its own behind a link (agent/remote.hpp).
#pragma once

#include "agent/agent.hpp"
#include "agent/video_pipeline.hpp"
#include "modules/machine_host.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace rcsdk::agent {

class MediaAgent final : public Port {
public:
    explicit MediaAgent(Events& events);
    ~MediaAgent() override;

    // Before the first start: the host's own pictures and a local preview of
    // the captured frames (embedded), and the host's own encoder (embedded, or
    // given to the agent of another process).
    void setHostCapture(VideoPipeline::HostCapture capture) { hostCapture_ = std::move(capture); }
    void setPreview(std::function<void(const VideoFrame&)> preview) { preview_ = std::move(preview); }
    void setHostEncoder(const rcs_host_encoder_config& config);

    Machine machine() const override;

    void startVideo(const VideoStart& start) override;
    void stopVideo(uint32_t index, uint32_t encoding) override;
    void setRate(uint32_t index, uint32_t encoding, uint32_t bitrate, bool budgeted) override;
    void setCursorInVideo(bool inVideo) override;
    void setInputBoost(uint32_t ms) override;
    void requestKeyframe(uint32_t index, uint32_t encoding) override;
    void invalidateAfter(uint32_t index, uint32_t encoding, uint64_t sequence) override;
    void setBacklogSource(std::function<uint64_t(uint32_t)> backlogNs) override;

    void startAudio(const AudioStart& start) override;
    void stopAudio() override;

    void configureInput(const platform::CaptureTarget& target, const std::vector<uint32_t>& streamDisplays,
                        bool secureDesktop) override;
    rcs_result applyInput(const rcs_input_event& event) override;
    void releaseInput() override;
    void startModule(const std::string& id, uint32_t version) override { modules_.start(id, version); }
    void stopModule(const std::string& id) override { modules_.stop(id); }
    void moduleData(const std::string& id, Bytes bytes) override { modules_.fromSession(id, std::move(bytes)); }


    std::vector<rcs_window_info> windows() override;

private:
    // Stream `index`'s pipeline, made at its first start with the settings so
    // far; none before (holding videoMutex_).
    VideoPipeline* pipeline(uint32_t index) const noexcept;
    void watch();
    void watchFailed(const std::string& problem);
    void checkInput();
    void checkDisplays();
    void checkDesktop();

    Events& events_;
    // Before the first start: the host's pictures, the preview and the host's encoder.
    VideoPipeline::HostCapture hostCapture_;
    std::function<void(const VideoFrame&)> preview_;
    std::optional<rcs_host_encoder_config> hostEncoder_;
    mutable std::mutex videoMutex_;
    std::array<std::unique_ptr<VideoPipeline>, RCS_MAX_STREAMS> video_; // By stream; made as they start.
    std::function<uint64_t(uint32_t)> backlog_;                         // What each pipeline is given...
    bool cursorInVideo_ = true;
    uint32_t boostMs_ = 0;
    uint32_t hostEncoders_ = 0;       // Machine::hostEncoders...
    uint32_t hostEncoderFormats_ = 0; // ...and hostEncoderFormats.

    std::mutex audioMutex_;
    std::unique_ptr<platform::AudioCapture> audio_;

    std::mutex inputMutex_;
    std::unique_ptr<platform::InputInjector> injector_;
    std::optional<platform::CaptureTarget> shared_; // What input maps across, its main display named.
    bool secureInput_ = false;                      // The injector follows secure desktops.
    int available_ = -1;                            // What was reported last: 0, 1, or -1 not yet.
    // Availability is worked out and reported by one thread at a time (the
    // watcher, a new injector), so a later report is never overtaken by an older one.
    std::mutex reportMutex_;

    // The modules' machine's halves (its thread starts with the first).
    modules::MachineHost modules_;

    std::mutex watchMutex_;
    std::condition_variable watchWake_;
    bool quit_ = false;
    std::vector<rcs_display_info> displays_;
    bool displaysKnown_ = false;
    std::optional<platform::DesktopState> desktop_; // As last reported.
    Clock::time_point nextInputCheck_{}, nextDisplayCheck_{};
    std::string watchProblem_; // As last logged.
    Clock::time_point nextWatchReport_{};

    std::thread watcher_; // Last: it starts once everything above exists.
};

} // namespace rcsdk::agent
