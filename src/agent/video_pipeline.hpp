// One of the agent's pictures (a stream, rcs_streams_config): capture, the
// main encoder fed from it, the bitrate budget and the loop between them, on
// a thread of its own, and the stream's lower encodings (RCS_EXT_ENCODINGS),
// each an encoder of its own fed the same pictures on a thread of its own. A
// start of encoding 0 brings up a new capture and main encoder for the
// stream (agent.hpp, VideoStart); the pipeline then captures, encodes and
// reports packets, the pointer and the capture's state until it is stopped
// or started again.
#pragma once

#include "agent/agent.hpp"
#include "media/layers.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace rcsdk::agent {

// Adaptive bitrate: what the encoder may produce. Encoders that cannot fit
// content into the target (fine detail changing everywhere) overshoot it;
// skipping capture while the budget is spent turns that into a lower frame
// rate instead of a path flooded with loss.
class EncodedBudget {
public:
    void setRate(uint32_t bitsPerSecond) noexcept { rate_ = bitsPerSecond * 1.3 / 8; }
    void spend(size_t bytes) noexcept { refill(), tokens_ -= double(bytes); }
    bool exhausted() noexcept { return refill(), rate_ > 0 && tokens_ < 0; }

private:
    void refill() noexcept;
    double rate_ = 0, tokens_ = 0; // Bytes per second; bytes, up to half a second's worth.
    Clock::time_point last_ = Clock::now();
};

// Back-pressure, as a blocking TCP send gives: while the transport still holds
// about two pictures of video (at least 40 ms), no new one is taken; the frame
// rate then follows what the path carries and latency stays low.
inline bool behind(uint64_t backlogNs, uint32_t framesPerSecond) noexcept {
    const auto frame = std::chrono::nanoseconds(1'000'000'000 / std::max(framesPerSecond, 1u));
    return std::chrono::nanoseconds(backlogNs) > std::max<std::chrono::nanoseconds>(2 * frame, 40ms);
}

// What an encoding takes from the stream's capture once it started: its
// encoder's device, where the shared picture sits and what it is.
struct CaptureView {
    std::shared_ptr<void> device;
    uint32_t width = 0, height = 0; // Of its frames...
    Rect content;                   // ...and where the shared picture sits in them.
    platform::DisplayMode display;
    rcs_hdr_metadata hdr{};
};

// A lower encoding of a stream: its own encoder, fed the stream's captured
// pictures at its own frame rate and size (made smaller where its size is)
// on a thread of its own, so that it never holds up the main encoding or the
// capture: a picture it is not ready for is replaced by the next. Its
// encoder opens on that thread; a failure there or later ends it alone
// (Events::encodingFailed).
class LowerEncoding {
public:
    // `backlogNs`: its viewers' send backlog (Port::setBacklogSource).
    LowerEncoding(Events& events, const VideoStart& start, const CaptureView& capture,
                  const std::optional<rcs_host_encoder_config>& hostEncoder, std::function<uint64_t()> backlogNs);
    ~LowerEncoding();
    LowerEncoding(const LowerEncoding&) = delete;
    LowerEncoding& operator=(const LowerEncoding&) = delete;

    // The latest captured picture (the pipeline's thread); one not taken yet is replaced.
    void offer(const VideoFrame& frame);
    // Whether it would take a picture now: its viewers are not behind.
    bool wants() const;
    void setRate(uint32_t bitrate, bool budgeted) noexcept { rate_ = (uint64_t(bitrate) << 1) | (budgeted ? 1 : 0); }
    void requestKeyframe() noexcept { keyframeRequested_ = true; }
    void invalidateAfter(uint64_t sequence) noexcept;
    // Time pictures were held back since it started, in nanoseconds.
    uint64_t limitedNs() const noexcept { return limitedNs_.load(); }

private:
    void run() noexcept;
    void open();
    void step(std::optional<VideoFrame> frame);
    void drain();
    void emit(EncodedPacket&& packet);

    Events& events_;
    const VideoStart start_;
    const CaptureView capture_;
    const std::optional<rcs_host_encoder_config> hostEncoder_;
    const std::function<uint64_t()> backlog_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<VideoFrame> offered_;
    bool quit_ = false;

    static constexpr uint64_t noRate = ~uint64_t(0);
    std::atomic<uint64_t> rate_{noRate};
    std::atomic<bool> keyframeRequested_{false};
    static constexpr uint64_t noInvalidation = ~uint64_t(0);
    std::atomic<uint64_t> invalidate_{noInvalidation};
    std::atomic<uint64_t> limitedNs_{0};

    // Its thread's.
    std::unique_ptr<platform::VideoEncoder> encoder_;
    std::unique_ptr<platform::SurfaceScaler> scaler_;
    EncodedBudget budget_;
    VideoFrame last_; // The latest picture offered: a keyframe of a still screen encodes it again.
    uint64_t lastOfferedNs_ = 0;
    uint64_t submitted_ = 0;
    double credit_ = 1;  // Pictures it may take now (its frame rate below the capture's).
    bool keyframePending_ = false, lowLatency_ = false, refInvalidation_ = false, hostEncoding_ = false;
    bool layerLogged_ = false;
    uint32_t layers_ = 1;      // The temporal layers its encoder makes...
    LayeredRecovery recovery_; // ...and the recoveries the lower ones wait for.
    Clock::time_point lastOutput_{};

    std::thread thread_; // Last.
};

class VideoPipeline {
public:
    using HostCapture = std::function<std::unique_ptr<platform::ScreenCapture>()>;

    VideoPipeline(Events& events, uint32_t index);
    ~VideoPipeline();
    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;

    // Before the first start: where the host's own pictures come from
    // (VideoStream::hostFrames), a local preview of each captured frame, and
    // the host's own encoder for the codecs it handles.
    void setHostCapture(HostCapture capture) { hostCapture_ = std::move(capture); }
    void setPreview(std::function<void(const VideoFrame&)> preview) { preview_ = std::move(preview); }
    void setHostEncoder(const rcs_host_encoder_config& config) { hostEncoder_ = config; }

    // The Port's video half (agent.hpp), each for one of the stream's encodings.
    void start(const VideoStart& start);
    void stop(uint32_t encoding);
    void setRate(uint32_t encoding, uint32_t bitrate, bool budgeted) noexcept;
    void setCursorInVideo(bool inVideo) noexcept { cursorInVideo_ = inVideo; }
    void setInputBoost(uint32_t ms) noexcept { boostMs_ = ms; }
    void requestKeyframe(uint32_t encoding) noexcept;
    // The earliest asked for since the last step goes to the encoder.
    void invalidateAfter(uint32_t encoding, uint64_t sequence) noexcept;
    void setBacklogSource(std::function<uint64_t(uint32_t)> backlogNs);
    // Input was executed: capture runs faster for a while.
    void boost() noexcept;

private:
    void run() noexcept;
    // A start of the stream. One whose encoder does not open while another
    // runs that captures the same (only its encoding changed: a codec, size,
    // frame rate or format the encoder does not take after all) leaves the
    // stream as it was: the one that ran starts again and the session hears
    // why (Events::encodingFailed of encoding 0). Anything else that fails
    // (the capture, what is shared changed) fails.
    void startOrKeep(const VideoStart& start);
    void restart(const VideoStart& start);
    void teardown() noexcept;
    void startLower(const VideoStart& start);
    void step();
    void drain();
    void emit(EncodedPacket&& packet);
    void emitCursor();
    void publishStatus(bool force);
    uint64_t backlogNs(uint32_t encoding);
    // Whether a lower encoding would take a picture now.
    bool lowerWants();

    Events& events_;
    const uint32_t index_; // The stream's.
    HostCapture hostCapture_;
    std::function<void(const VideoFrame&)> preview_;
    std::optional<rcs_host_encoder_config> hostEncoder_;

    // The caller's side of the pipeline: a start or a stop waiting to be
    // taken, and each lower encoding's.
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<VideoStart> pendingStart_;
    bool pendingStop_ = false;
    std::array<std::optional<VideoStart>, RCS_MAX_ENCODINGS> pendingLower_; // A start...
    uint32_t pendingLowerStops_ = 0;                                         // ...or a stop, as (1u << encoding) bits.
    bool quit_ = false;
    std::atomic<bool> pending_{false};
    std::atomic<bool> cancelled_{false};

    std::mutex backlogMutex_;
    std::function<uint64_t(uint32_t)> backlogSource_;

    static constexpr uint64_t noRate = ~uint64_t(0);
    std::atomic<uint64_t> rate_{noRate}; // bitrate << 1 | budgeted, until the pipeline takes it.
    std::atomic<bool> keyframeRequested_{false};
    static constexpr uint64_t noInvalidation = ~uint64_t(0);
    std::atomic<uint64_t> invalidate_{noInvalidation}; // invalidateAfter's, until the pipeline takes it.
    std::atomic<bool> cursorInVideo_{true};
    std::atomic<uint32_t> boostMs_{0};
    std::atomic<uint64_t> boostUntilNs_{0};

    // The lower encodings, made and ended on the pipeline's thread; the
    // Port's calls reach them under lowerMutex_.
    std::mutex lowerMutex_;
    std::array<std::unique_ptr<LowerEncoding>, RCS_MAX_ENCODINGS> lower_; // 1 and 2.

    // The pipeline thread's.
    bool running_ = false;
    std::optional<VideoStart> started_; // What runs.
    VideoStream stream_;
    std::unique_ptr<platform::ScreenCapture> capture_;
    std::unique_ptr<platform::VideoEncoder> encoder_;
    EncodedBudget budget_;
    static constexpr auto encoderSilence = std::chrono::seconds(5);
    Clock::time_point lastOutput_{}; // The encoder's latest packet, or the stream's start.
    VideoFrame lastFrame_;
    bool keyframePending_ = false, lowLatency_ = false, refInvalidation_ = false, cursorInVideoApplied_ = true;
    bool hostEncoding_ = false; // The host's encoder encodes this stream.
    bool layerLogged_ = false;  // The host's encoder said another temporal layer than its bitstream (logged once).
    uint32_t layers_ = 1;       // The temporal layers its encoder makes...
    LayeredRecovery recovery_;  // ...and the recoveries the lower ones wait for.
    uint64_t submitted_ = 0;
    Rect announcedContent_;
    platform::DisplayMode announcedDisplay_;
    CaptureStatus status_;
    uint64_t limitedNs_ = 0; // Pictures held back: the budget spent, the path behind (all its streams).
    std::optional<CursorState> sentCursor_;

    std::thread thread_; // Last: it starts once everything above exists.
};

// Opens the encoder a stream's encoding is to use: the host's own where it
// handles the codec, else the SDK's (its acceleration decided for the GPU of
// `device`). Returns it opened, with the acceleration it is reported with.
std::pair<std::unique_ptr<platform::VideoEncoder>, rcs_acceleration>
openEncoder(const VideoStart& start, const std::shared_ptr<void>& device,
            const std::optional<rcs_host_encoder_config>& hostEncoder);

} // namespace rcsdk::agent
