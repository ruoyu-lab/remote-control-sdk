#include "agent/video_pipeline.hpp"

#include "agent/host_encoder.hpp"
#include "codecs/codecs.hpp"
#include "media/layers.hpp"
#include "media/sampling.hpp"
#include "media/scale.hpp"
#include "seams/seams.hpp"
#include "util/log.hpp"

#include <algorithm>

namespace rcsdk::agent {
namespace {

// The stream's encoder did not open (VideoPipeline::startOrKeep).
class EncoderRefused final : public Failure {
public:
    explicit EncoderRefused(const Failure& failure) : Failure(failure.result(), failure.what()) {}
};

} // namespace

void EncodedBudget::refill() noexcept {
    const auto now = Clock::now();
    tokens_ = std::min(rate_ * 0.5, tokens_ + rate_ * std::chrono::duration<double>(now - last_).count());
    last_ = now;
}

std::pair<std::unique_ptr<platform::VideoEncoder>, rcs_acceleration>
openEncoder(const VideoStart& start, const std::shared_ptr<void>& device,
            const std::optional<rcs_host_encoder_config>& hostEncoder) {
    const auto& s = start.stream;
    const bool host = hostEncoder && handles(*hostEncoder, s.codec);
    platform::EncoderConfig encoding;
    encoding.codec = s.codec;
    encoding.width = s.width;
    encoding.height = s.height;
    encoding.framesPerSecond = s.framesPerSecond;
    encoding.bitrate = start.bitrate;
    encoding.device = device;
    // HARDWARE_FIRST: the capturing GPU's hardware encoder where it takes the stream.
    encoding.acceleration =
        codecs::streamAcceleration(s.acceleration, s.backend, s.codec, RCS_CODEC_ENCODER, s.picture.format, s.width,
                                   s.height, platform::deviceAdapter(device), codecImplementations());
    encoding.backend = s.backend;
    encoding.picture = s.picture;
    encoding.speed = s.speed;
    encoding.content = s.content != RCS_CONTENT_AUTO ? s.content
                       : s.hostFrames                ? rcs_content_hint(RCS_CONTENT_MOTION)
                                                     : rcs_content_hint(RCS_CONTENT_DETAIL);
    encoding.softwareThreads = s.softwareThreads;
    encoding.temporalLayers = s.temporalLayers;
    std::unique_ptr<platform::VideoEncoder> encoder =
        host ? std::make_unique<HostEncoder>(*hostEncoder, start.index, start.encoding)
             : codecs::createEncoder(encoding, codecImplementations());
    encoder->open(encoding);
    return {std::move(encoder), host ? hostEncoder->acceleration : encoding.acceleration};
}

// ---- A lower encoding --------------------------------------------------------------------------

LowerEncoding::LowerEncoding(Events& events, const VideoStart& start, const CaptureView& capture,
                             const std::optional<rcs_host_encoder_config>& hostEncoder,
                             std::function<uint64_t()> backlogNs)
    : events_(events), start_(start), capture_(capture), hostEncoder_(hostEncoder), backlog_(std::move(backlogNs)) {
    thread_ = log::thread([this] { run(); });
}

LowerEncoding::~LowerEncoding() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void LowerEncoding::offer(const VideoFrame& frame) {
    {
        std::lock_guard lock(mutex_);
        offered_ = frame;
    }
    wake_.notify_one();
}

bool LowerEncoding::wants() const {
    return !behind(backlog_(), start_.stream.framesPerSecond);
}

void LowerEncoding::invalidateAfter(uint64_t sequence) noexcept {
    uint64_t current = invalidate_.load();
    while (sequence < current && !invalidate_.compare_exchange_weak(current, sequence)) {
    }
}

void LowerEncoding::run() noexcept {
    try {
        open();
        for (;;) {
            std::optional<VideoFrame> frame;
            {
                std::unique_lock lock(mutex_);
                wake_.wait_for(lock, 20ms, [&] { return quit_ || offered_; });
                if (quit_) {
                    break;
                }
                frame = std::exchange(offered_, std::nullopt);
            }
            step(std::move(frame));
        }
    } catch (const Failure& failure) {
        events_.encodingFailed(start_.index, start_.encoding, failure.result(), failure.what());
    } catch (const std::exception& exception) {
        events_.encodingFailed(start_.index, start_.encoding, RCS_ERROR_INTERNAL, exception.what());
    }
    encoder_.reset();
}

void LowerEncoding::open() {
    const auto& s = start_.stream;
    auto [encoder, acceleration] = openEncoder(start_, capture_.device, hostEncoder_);
    encoder_ = std::move(encoder);
    hostEncoding_ = encoder_->backend() == RCS_BACKEND_HOST;
    lowLatency_ = encoder_->lowLatency();
    refInvalidation_ = encoder_->refInvalidation();
    layers_ = encoder_->temporalLayers();
    budget_.setRate(start_.budgeted ? start_.bitrate : 0);
    lastOutput_ = Clock::now();
    // Where the shared picture sits in this encoding's frame, as in the capture's.
    const auto& c = capture_.content;
    const auto x = [&](uint32_t value) { return uint32_t(uint64_t(value) * s.width / std::max(capture_.width, 1u)); };
    const auto y = [&](uint32_t value) { return uint32_t(uint64_t(value) * s.height / std::max(capture_.height, 1u)); };
    const Rect content{x(c.x), y(c.y), std::max(x(c.width), 1u), std::max(y(c.height), 1u)};
    events_.videoStarted({start_.index, start_.encoding, s, content, capture_.display, acceleration,
                          encoder_->backend(), lowLatency_, encoder_->name(), capture_.hdr, refInvalidation_, layers_});
}

void LowerEncoding::step(std::optional<VideoFrame> frame) {
    const auto& s = start_.stream;
    if (const uint64_t rate = rate_.exchange(noRate); rate != noRate) {
        encoder_->setBitrate(uint32_t(rate >> 1));
        budget_.setRate((rate & 1) ? uint32_t(rate >> 1) : 0);
    }
    if (keyframeRequested_.exchange(false)) {
        encoder_->requestKeyframe();
        keyframePending_ = true;
    }
    if (const uint64_t after = invalidate_.exchange(noInvalidation); after != noInvalidation) {
        encoder_->invalidateAfter(after);
        keyframePending_ = true;
    }
    drain();
    if (const size_t held = encoder_->inFlight(); held >= 3) {
        if (EncodedPacket packet; encoder_->receive(packet, 20ms)) {
            emit(std::move(packet));
        } else if (Clock::now() - lastOutput_ > 5s) {
            fail(RCS_ERROR_CODEC, encoder_->name() + " gave no picture for 5 s with " + std::to_string(held) +
                                      " frames in it");
        }
        return;
    }
    if (frame) {
        // Its frame rate: credit for pictures as time passes, half one more
        // at most, so that over time no more than its rate go.
        const uint64_t since = lastOfferedNs_ ? frame->timestampNs - std::min(frame->timestampNs, lastOfferedNs_) : 0;
        credit_ = std::min(1.5, credit_ + double(since) * s.framesPerSecond / 1e9);
        lastOfferedNs_ = frame->timestampNs;
        last_ = *frame;
        if (credit_ < 1 && !keyframePending_) {
            return;
        }
        if (behind(backlog_(), s.framesPerSecond) || (!keyframePending_ && budget_.exhausted())) {
            limitedNs_ += since;
            return;
        }
        credit_ = std::max(0.0, credit_ - 1);
    } else {
        // A still screen offers nothing; a keyframe asked for still goes.
        if (!keyframePending_ || !last_.width) {
            return;
        }
        frame = last_;
        frame->timestampNs = monotonicNs();
    }
    keyframePending_ = false;
    VideoFrame picture = std::move(*frame);
    if (picture.width != s.width || picture.height != s.height) {
        if (picture.pixels && !picture.surface) {
            picture = scaleBgra(picture, s.width, s.height);
        } else {
            if (!scaler_) {
                scaler_ = platform::createSurfaceScaler();
            }
            picture = scaler_->scale(picture, s.width, s.height);
        }
    }
    picture.stream = start_.index;
    picture.sequence = ++submitted_; // Each encoding numbers its own pictures.
    encoder_->submit(picture);
    drain();
}

void LowerEncoding::drain() {
    for (EncodedPacket packet; encoder_->receive(packet, 0ms);) {
        emit(std::move(packet));
    }
}

void LowerEncoding::emit(EncodedPacket&& packet) {
    packet.stream = start_.index;
    packet.encoding = start_.encoding;
    packet.picture = start_.stream.picture;
    if (!hostEncoding_) {
        if (const auto problem = samplingProblem(packet); !problem.empty()) {
            fail(RCS_ERROR_CODEC, encoder_->name() + " produced a keyframe with " + problem);
        }
    }
    if (readTemporalLayer(packet) && hostEncoding_ && !layerLogged_) {
        layerLogged_ = true;
        log::warning("encoder", "The host's encoder says another temporal layer than its bitstream; the "
                                "bitstream's is sent");
    }
    recovery_.apply(packet);
    budget_.spend(packet.data.size() + packet.config.size());
    lastOutput_ = Clock::now();
    if (encoder_->lowLatency() != lowLatency_ || encoder_->refInvalidation() != refInvalidation_ ||
        encoder_->temporalLayers() != layers_) {
        lowLatency_ = encoder_->lowLatency();
        refInvalidation_ = encoder_->refInvalidation();
        layers_ = encoder_->temporalLayers();
        events_.encoderMode(start_.index, start_.encoding, packet.codec, lowLatency_, refInvalidation_, layers_,
                            encoder_->name());
    }
    events_.videoPacket(std::move(packet));
}

// ---- The pipeline ------------------------------------------------------------------------------

VideoPipeline::VideoPipeline(Events& events, uint32_t index) : events_(events), index_(index) {
    thread_ = log::thread([this] { run(); });
}

VideoPipeline::~VideoPipeline() {
    cancelled_ = true;
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void VideoPipeline::start(const VideoStart& start) {
    if (start.encoding) {
        std::lock_guard lock(mutex_);
        pendingLower_[start.encoding] = start;
        pendingLowerStops_ &= ~(1u << start.encoding);
        pending_ = true;
        wake_.notify_one();
        return;
    }
    rate_ = noRate; // Rates asked for before this start belong to the stream it replaces.
    // Set here, not when the pipeline takes the start: a later setCursorInVideo
    // or setInputBoost made while the stream starts must not be undone by it.
    cursorInVideo_ = start.cursorInVideo;
    boostMs_ = start.inputBoostMs;
    {
        std::lock_guard lock(mutex_);
        pendingStart_ = start;
        pendingStop_ = false;
        pendingLower_ = {}; // The stream's lower encodings start again after it.
        pendingLowerStops_ = 0;
        pending_ = true;
    }
    wake_.notify_one();
}

void VideoPipeline::stop(uint32_t encoding) {
    {
        std::lock_guard lock(mutex_);
        if (encoding) {
            pendingLower_[encoding].reset();
            pendingLowerStops_ |= 1u << encoding;
        } else {
            pendingStart_.reset();
            pendingStop_ = true;
            pendingLower_ = {};
            pendingLowerStops_ = 0;
        }
        pending_ = true;
    }
    wake_.notify_one();
}

void VideoPipeline::setRate(uint32_t encoding, uint32_t bitrate, bool budgeted) noexcept {
    if (!encoding) {
        rate_ = (uint64_t(bitrate) << 1) | (budgeted ? 1 : 0);
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (auto& pending = pendingLower_[encoding]) {
            pending->bitrate = bitrate; // It starts at it.
            pending->budgeted = budgeted;
        }
    }
    std::lock_guard lock(lowerMutex_);
    if (const auto& lower = lower_[encoding]) {
        lower->setRate(bitrate, budgeted);
    }
}

void VideoPipeline::requestKeyframe(uint32_t encoding) noexcept {
    if (!encoding) {
        keyframeRequested_ = true;
        return;
    }
    std::lock_guard lock(lowerMutex_);
    if (const auto& lower = lower_[encoding]) {
        lower->requestKeyframe();
    }
}

void VideoPipeline::invalidateAfter(uint32_t encoding, uint64_t sequence) noexcept {
    if (encoding) {
        std::lock_guard lock(lowerMutex_);
        if (const auto& lower = lower_[encoding]) {
            lower->invalidateAfter(sequence);
        }
        return;
    }
    uint64_t current = invalidate_.load();
    while (sequence < current && !invalidate_.compare_exchange_weak(current, sequence)) {
    }
}

void VideoPipeline::setBacklogSource(std::function<uint64_t(uint32_t)> backlogNs) {
    std::lock_guard lock(backlogMutex_);
    backlogSource_ = std::move(backlogNs);
}

uint64_t VideoPipeline::backlogNs(uint32_t encoding) {
    std::lock_guard lock(backlogMutex_);
    return backlogSource_ ? backlogSource_(encoding) : 0;
}

void VideoPipeline::boost() noexcept {
    if (const uint32_t ms = boostMs_) {
        boostUntilNs_ = monotonicNs() + uint64_t(ms) * 1'000'000;
    }
}

// ---- The pipeline's thread ---------------------------------------------------------------------

void VideoPipeline::run() noexcept {
    while (!cancelled_) {
        std::optional<VideoStart> start;
        std::array<std::optional<VideoStart>, RCS_MAX_ENCODINGS> lowerStarts;
        uint32_t lowerStops = 0;
        bool stop = false;
        if (pending_ || !running_) {
            std::unique_lock lock(mutex_);
            if (!pending_ && !quit_) { // Idle: waits for a start.
                wake_.wait_for(lock, 50ms, [&] { return pending_.load() || quit_; });
            }
            if (quit_) {
                break;
            }
            if (pending_) {
                start = std::move(pendingStart_);
                pendingStart_.reset();
                stop = pendingStop_;
                pendingStop_ = false;
                // Lower encodings take pictures from a capture: without one
                // they wait for the next start of the stream (which replaces them).
                if (running_ || start) {
                    lowerStarts = std::exchange(pendingLower_, {});
                    lowerStops = std::exchange(pendingLowerStops_, 0);
                }
                pending_ = false;
            }
        }
        try {
            if (stop) {
                teardown();
            }
            if (start) {
                startOrKeep(*start);
            }
            for (uint32_t encoding = 1; encoding < RCS_MAX_ENCODINGS; ++encoding) {
                if (lowerStops & (1u << encoding)) {
                    std::unique_ptr<LowerEncoding> ended;
                    {
                        std::lock_guard lock(lowerMutex_);
                        ended = std::move(lower_[encoding]);
                    }
                }
                if (lowerStarts[encoding] && running_) {
                    startLower(*lowerStarts[encoding]);
                }
            }
            if (running_) {
                step();
            }
        } catch (const Failure& failure) {
            teardown();
            events_.failed(failure.result(), failure.what());
        } catch (const std::exception& exception) {
            teardown();
            events_.failed(RCS_ERROR_INTERNAL, exception.what());
        }
    }
    teardown();
}

void VideoPipeline::teardown() noexcept {
    std::array<std::unique_ptr<LowerEncoding>, RCS_MAX_ENCODINGS> lower;
    {
        std::lock_guard lock(lowerMutex_);
        lower = std::move(lower_);
    }
    for (auto& encoding : lower) {
        encoding.reset(); // Its thread ends, unlocked: its events may call the Port.
    }
    if (capture_) {
        try {
            publishStatus(true);
        } catch (const std::exception& error) { // The stream ends all the same.
            log::warning("capture", std::string("The capture's last status was not reported: ") + error.what());
        }
    }
    encoder_.reset(); // Before the capture its output callback wakes.
    capture_.reset();
    running_ = false;
    started_.reset();
    lastFrame_ = {};
}

void VideoPipeline::startOrKeep(const VideoStart& start) {
    const auto previous = running_ ? started_ : std::nullopt;
    try {
        restart(start);
    } catch (const EncoderRefused& failure) {
        const auto& was = previous ? previous->stream : start.stream;
        const auto& now = start.stream;
        if (!previous || was == now || was.hostFrames != now.hostFrames || !(was.target == now.target) ||
            was.excludeCurrentProcess != now.excludeCurrentProcess || was.secureDesktop != now.secureDesktop) {
            throw;
        }
        log::warning("capture", "Stream " + std::to_string(index_) + " stays as it was: " + failure.what());
        events_.encodingFailed(index_, 0, failure.result(), failure.what());
        restart(*previous);
    }
}

void VideoPipeline::restart(const VideoStart& start) {
    if (encoder_) { // What the old encoder still holds goes out first.
        for (EncodedPacket packet; encoder_->inFlight() && encoder_->receive(packet, 50ms);) {
            emit(std::move(packet));
        }
    }
    teardown();
    const auto& s = start.stream;
    if (s.hostFrames && !hostCapture_) {
        fail(RCS_ERROR_INVALID_STATE, "This agent has no source for the host's own pictures");
    }
    capture_ = s.hostFrames ? hostCapture_() : seams::createScreenCapture(s.target);
    const bool cursorInVideo = cursorInVideo_;
    capture_->start({s.width, s.height, s.framesPerSecond, s.target, s.excludeCurrentProcess, cursorInVideo,
                     s.secureDesktop, s.picture},
                    cancelled_);
    std::pair<std::unique_ptr<platform::VideoEncoder>, rcs_acceleration> opened;
    try {
        opened = openEncoder(start, capture_->device(), hostEncoder_);
    } catch (const Failure& failure) {
        if (failure.result() == RCS_ERROR_CANCELLED) {
            throw;
        }
        throw EncoderRefused(failure);
    }
    auto [encoder, acceleration] = std::move(opened);
    encoder_ = std::move(encoder);
    hostEncoding_ = encoder_->backend() == RCS_BACKEND_HOST;
    encoder_->onOutputReady([capture = capture_.get()] { capture->interrupt(); });
    stream_ = s;
    running_ = true;
    budget_.setRate(start.budgeted ? start.bitrate : 0);
    cursorInVideoApplied_ = cursorInVideo;
    keyframeRequested_ = false;
    keyframePending_ = false;
    lastOutput_ = Clock::now();
    lowLatency_ = encoder_->lowLatency();
    refInvalidation_ = encoder_->refInvalidation();
    layers_ = encoder_->temporalLayers();
    announcedContent_ = capture_->content();
    announcedDisplay_ = capture_->display();
    sentCursor_.reset();
    status_ = {};
    limitedNs_ = 0;
    started_ = start;
    events_.videoStarted({index_, 0, s, announcedContent_, announcedDisplay_, acceleration, encoder_->backend(),
                          lowLatency_, encoder_->name(), capture_->hdrMetadata(), refInvalidation_, layers_});
}

void VideoPipeline::startLower(const VideoStart& start) {
    std::unique_ptr<LowerEncoding> replaced;
    {
        std::lock_guard lock(lowerMutex_);
        replaced = std::move(lower_[start.encoding]);
    }
    replaced.reset(); // Ends first, unlocked (its events may call the Port); what it held is not sent.
    auto lower = std::make_unique<LowerEncoding>(
        events_, start,
        CaptureView{capture_->device(), stream_.width, stream_.height, announcedContent_, announcedDisplay_,
                    capture_->hdrMetadata()},
        hostEncoder_, [this, encoding = start.encoding] { return backlogNs(encoding); });
    if (lastFrame_.width) { // A still screen captures nothing new: it starts from the latest picture.
        VideoFrame latest = lastFrame_;
        latest.timestampNs = monotonicNs();
        lower->offer(latest);
    }
    std::lock_guard lock(lowerMutex_);
    lower_[start.encoding] = std::move(lower);
}

bool VideoPipeline::lowerWants() {
    std::lock_guard lock(lowerMutex_);
    return std::any_of(lower_.begin(), lower_.end(), [](const auto& lower) { return lower && lower->wants(); });
}

void VideoPipeline::step() {
    if (const uint64_t rate = rate_.exchange(noRate); rate != noRate) {
        encoder_->setBitrate(uint32_t(rate >> 1));
        budget_.setRate((rate & 1) ? uint32_t(rate >> 1) : 0);
    }
    if (cursorInVideo_ != cursorInVideoApplied_) {
        cursorInVideoApplied_ = cursorInVideo_;
        capture_->setCursorInVideo(cursorInVideoApplied_);
    }
    if (keyframeRequested_.exchange(false)) {
        encoder_->requestKeyframe();
        keyframePending_ = true;
    }
    if (const uint64_t after = invalidate_.exchange(noInvalidation); after != noInvalidation) {
        encoder_->invalidateAfter(after);
        keyframePending_ = true; // A picture is due, from a still screen too.
    }
    capture_->boost(boostUntilNs_);
    drain();
    // Let the encoder catch up before capturing more. Three frames keep a
    // pipelined hardware encoder busy: VideoToolbox HEVC needs ~18 ms per
    // 4K frame but sustains 120 FPS when frames overlap.
    if (const size_t held = encoder_->inFlight(); held >= 3) {
        if (EncodedPacket packet; encoder_->receive(packet, 20ms)) {
            emit(std::move(packet));
        } else if (Clock::now() - lastOutput_ > encoderSilence) {
            // An encoder that takes frames and gives nothing (a hung
            // MediaCodec, VideoToolbox's callback never coming) would stall
            // the stream unseen. The slowest that works, software at 4K or a
            // phone's 8K keyframe, gives a picture within a second.
            fail(RCS_ERROR_CODEC, encoder_->name() + " gave no picture for " +
                                      std::to_string(encoderSilence.count()) + " s with " + std::to_string(held) +
                                      " frames in it");
        }
        return;
    }
    // Back-pressure (behind()): while the main encoding's viewers are behind,
    // it takes nothing new, and capture waits unless a lower encoding still
    // takes pictures; look again in 2 ms (a plain sleep would take a 15.6 ms
    // tick on Windows).
    const bool held = behind(backlogNs(0), stream_.framesPerSecond) || (!keyframePending_ && budget_.exhausted());
    const auto stepped = Clock::now();
    if (held && !lowerWants()) {
        sleepPrecise(2ms);
        limitedNs_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - stepped).count());
        publishStatus(false);
        return;
    }

    VideoFrame frame;
    const bool fresh = capture_->next(frame, 20ms, cancelled_);
    publishStatus(false);
    emitCursor();
    if (fresh && (capture_->content() != announcedContent_ || capture_->display() != announcedDisplay_)) {
        // A window resized, or capture resumed after a display mode change:
        // what is shared sits differently in the frame. The session announces
        // that before the frame itself.
        announcedContent_ = capture_->content();
        announcedDisplay_ = capture_->display();
        events_.videoContent(index_, announcedContent_, announcedDisplay_);
    }
    if (fresh) {
        frame.stream = index_;
        std::lock_guard lock(lowerMutex_);
        for (const auto& lower : lower_) {
            if (lower) {
                lower->offer(frame);
            }
        }
    }
    if (held) {
        limitedNs_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - stepped).count());
        if (fresh) {
            lastFrame_ = frame;
        }
        return;
    }
    if (!fresh) {
        // A static screen produces no frames; re-encode the last one so a
        // requested keyframe still reaches the viewer.
        if (!keyframePending_ || !lastFrame_.width) {
            return;
        }
        frame = lastFrame_;
        frame.timestampNs = monotonicNs();
    }
    keyframePending_ = false;
    frame.sequence = ++submitted_;
    encoder_->submit(frame);
    lastFrame_ = frame;
    if (preview_) {
        preview_(frame); // The host's local preview, if it asked for frames.
    }
    drain();
}

void VideoPipeline::drain() {
    for (EncodedPacket packet; encoder_->receive(packet, 0ms);) {
        emit(std::move(packet));
    }
}

void VideoPipeline::emit(EncodedPacket&& packet) {
    packet.stream = index_;
    packet.encoding = 0;
    packet.picture = stream_.picture; // What the encoder was opened for...
    if (!hostEncoding_) {             // ...and what it encodes (the host's packets were checked as pushed).
        if (const auto problem = samplingProblem(packet); !problem.empty()) {
            fail(RCS_ERROR_CODEC, encoder_->name() + " produced a keyframe with " + problem);
        }
    }
    if (readTemporalLayer(packet) && hostEncoding_ && !layerLogged_) {
        layerLogged_ = true;
        log::warning("encoder", "The host's encoder says another temporal layer than its bitstream; the "
                                "bitstream's is sent");
    }
    recovery_.apply(packet);
    budget_.spend(packet.data.size() + packet.config.size());
    lastOutput_ = Clock::now();
    if (encoder_->lowLatency() != lowLatency_ || encoder_->refInvalidation() != refInvalidation_ ||
        encoder_->temporalLayers() != layers_) {
        lowLatency_ = encoder_->lowLatency();
        refInvalidation_ = encoder_->refInvalidation();
        layers_ = encoder_->temporalLayers();
        events_.encoderMode(index_, 0, packet.codec, lowLatency_, refInvalidation_, layers_, encoder_->name());
    }
    events_.videoPacket(std::move(packet));
}

void VideoPipeline::emitCursor() {
    CursorState state = capture_->cursor();
    if (sentCursor_ && state.visible == sentCursor_->visible && state.x == sentCursor_->x &&
        state.y == sentCursor_->y && state.imageSerial == sentCursor_->imageSerial) {
        return;
    }
    sentCursor_ = state;
    events_.cursor(index_, state);
}

// The capture's counts and whether it is paused, and each encoding's time
// held back, to the session, whenever they change.
void VideoPipeline::publishStatus(bool force) {
    CaptureStatus status{capture_->paused(), capture_->captured(), capture_->overwritten(), capture_->interruptions(),
                         {}};
    status.limitedMs[0] = limitedNs_ / 100'000'000 * 100;
    {
        std::lock_guard lock(lowerMutex_);
        for (uint32_t encoding = 1; encoding < RCS_MAX_ENCODINGS; ++encoding) {
            if (const auto& lower = lower_[encoding]) {
                status.limitedMs[encoding] = lower->limitedNs() / 100'000'000 * 100;
            }
        }
    }
    if (force || status != status_) {
        status_ = status;
        events_.captureStatus(index_, status);
    }
}

} // namespace rcsdk::agent
