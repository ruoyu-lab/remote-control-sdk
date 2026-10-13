#include "session/host_sources.hpp"

#include "media/audio.hpp"
#include "media/encoder_output.hpp"
#include "media/layers.hpp"
#include "media/sampling.hpp"
#include "protocol/messages.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

namespace rcsdk::session {

class HostVideoCapture final : public platform::ScreenCapture {
public:
    explicit HostVideoCapture(HostVideo& video) : video_(video) {}
    ~HostVideoCapture() override { stop(); }

    void start(const platform::CaptureConfig& config, CancelToken) override {
        std::lock_guard lock(video_.mutex_);
        video_.width_ = config.width;
        video_.height_ = config.height;
        video_.latest_.reset();
        display_ = {};
        pace(config.framesPerSecond);
        content_ = {0, 0, config.width, config.height};
    }

    // The newest pushed frame, no more often than the frame rate.
    bool next(VideoFrame& frame, std::chrono::milliseconds timeout, CancelToken cancelled) override {
        std::unique_lock lock(video_.mutex_);
        const auto deadline = Clock::now() + timeout;
        video_.arrived_.wait_until(lock, deadline, [&] {
            return video_.latest_ || video_.interrupted_ || cancelled.load();
        });
        const auto slot = due();
        if (video_.latest_ && Clock::now() < slot) {
            video_.arrived_.wait_until(lock, std::min(slot, deadline),
                                       [&] { return video_.interrupted_ || cancelled.load(); });
        }
        video_.interrupted_ = false;
        if (!video_.latest_ || Clock::now() < slot) {
            return false;
        }
        frame = std::move(*video_.latest_);
        video_.latest_.reset();
        content_ = video_.latestContent_;
        display_ = video_.latestDisplay_;
        took();
        return true;
    }

    void interrupt() noexcept override {
        std::lock_guard lock(video_.mutex_);
        video_.interrupted_ = true;
        video_.arrived_.notify_all();
    }

    void stop() noexcept override {
        std::lock_guard lock(video_.mutex_);
        video_.width_ = video_.height_ = 0;
        video_.latest_.reset();
    }

    uint64_t captured() const noexcept override {
        std::lock_guard lock(video_.mutex_);
        return video_.pushed_;
    }
    uint64_t overwritten() const noexcept override {
        std::lock_guard lock(video_.mutex_);
        return video_.replaced_;
    }
    void setCursorInVideo(bool) override {} // The host draws what it pushes.
    CursorState cursor() const override { return {}; }
    // The last picture taken's.
    Rect content() const noexcept override { return content_; }
    platform::DisplayMode display() const noexcept override { return display_; }

private:
    HostVideo& video_;
    Rect content_;
    platform::DisplayMode display_;
};

std::unique_ptr<platform::ScreenCapture> HostVideo::capture() {
    return std::make_unique<HostVideoCapture>(*this);
}

void HostVideo::push(const rcs_frame& frame) {
    if (frame.format != RCS_PIXEL_BGRA8 || !frame.data || frame.stride_bytes < uint64_t(frame.width) * 4 ||
        frame.data_size < uint64_t(frame.stride_bytes) * frame.height) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "The frame must be BGRA8, with data of stride_bytes (at least width * 4) "
                                         "times height");
    }
    std::lock_guard lock(mutex_);
    if (!width_) {
        fail(RCS_ERROR_INVALID_STATE, "No stream is running");
    }
    if (frame.width != width_ || frame.height != height_) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "The frame is " + std::to_string(frame.width) + "x" +
                                             std::to_string(frame.height) + ", the stream " +
                                             std::to_string(width_) + "x" + std::to_string(height_));
    }
    auto pixels = std::make_shared<std::vector<uint8_t>>(size_t(width_) * height_ * 4);
    for (uint32_t y = 0; y < height_; ++y) {
        std::memcpy(pixels->data() + size_t(y) * width_ * 4, frame.data + size_t(y) * frame.stride_bytes,
                    size_t(width_) * 4);
    }
    VideoFrame copy;
    copy.width = width_;
    copy.height = height_;
    copy.stride = width_ * 4;
    copy.pixels = std::move(pixels);
    copy.timestampNs = monotonicNs();
    keep(std::move(copy), {0, 0, width_, height_}, {});
}

void HostVideo::push(const rcs_gpu_frame_info& frame) {
    VideoFrame image = platform::hostGpuFrame(frame); // Retained: the host may let go of it now.
    std::lock_guard lock(mutex_);
    if (!width_) {
        fail(RCS_ERROR_INVALID_STATE, "No stream is running");
    }
    if (image.width != width_ || image.height != height_) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "The picture is " + std::to_string(image.width) + "x" +
                                             std::to_string(image.height) + ", the stream " +
                                             std::to_string(width_) + "x" + std::to_string(height_));
    }
    keep(std::move(image), {0, 0, width_, height_}, {});
}

void HostVideo::prepareSamples() {
    std::lock_guard fitting(fitting_);
    if (!samples_) {
        samples_ = platform::createSamplePictures();
    }
}

void HostVideo::pushSample(const void* image, uint32_t orientation) {
    prepareSamples();
    std::lock_guard fitting(fitting_); // On the GPU, without holding up the encoder's wait.
    uint32_t width = 0, height = 0;
    {
        std::lock_guard lock(mutex_);
        if (!width_) {
            fail(RCS_ERROR_INVALID_STATE, "No stream is running");
        }
        width = width_;
        height = height_;
    }
    auto picture = samples_->fit(image, orientation, width, height);
    std::lock_guard lock(mutex_);
    if (width_ != width || height_ != height) {
        fail(RCS_ERROR_INVALID_STATE, "The stream restarted meanwhile");
    }
    keep(std::move(picture.frame), picture.content, picture.size);
}

void HostVideo::keep(VideoFrame frame, const Rect& content, const platform::DisplayMode& display) {
    latestContent_ = content;
    latestDisplay_ = display;
    frame.sequence = ++sequence_;
    replaced_ += latest_.has_value();
    ++pushed_;
    latest_ = std::move(frame);
    arrived_.notify_all();
}

class HostAudioCapture final : public platform::AudioCapture {
public:
    explicit HostAudioCapture(HostAudio& audio) : audio_(audio) {}
    ~HostAudioCapture() override { stop(); }

    void start(const platform::AudioCaptureConfig&, Deliver deliver, Status status) override {
        {
            std::lock_guard lock(audio_.mutex_);
            audio_.deliver_ = std::move(deliver);
            ++audio_.captures_;
        }
        status({true, RCS_OK, {}});
    }
    void stop() noexcept override {
        std::lock_guard lock(audio_.mutex_);
        audio_.deliver_ = {};
    }
    uint32_t periodFrames() const noexcept override { return audio_.lastFrames_; }

private:
    HostAudio& audio_;
};

std::unique_ptr<platform::AudioCapture> HostAudio::capture() {
    return std::make_unique<HostAudioCapture>(*this);
}

void HostAudio::push(const rcs_audio_frame& frame) {
    if (frame.sample_rate != audioRate || frame.channels != audioChannels || !frame.samples || !frame.frames ||
        frame.frames > audioRate) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "Sound must be 48 kHz stereo float32 samples, 1..48 000 frames");
    }
    std::lock_guard lock(mutex_); // Delivery runs under it: the sender cannot stop mid-packet.
    if (!deliver_) {
        fail(RCS_ERROR_INVALID_STATE, "No sound is being sent");
    }
    lastFrames_ = frame.frames;
    deliver_(std::span(frame.samples, size_t(frame.frames) * audioChannels), monotonicNs());
}

void HostAudio::prepareSamples() {
    std::lock_guard converting(converting_);
    for (auto& samples : samples_) {
        if (!samples) {
            samples = platform::createSampleSound();
        }
    }
}

void HostAudio::pushSample(const void* sampleBuffer, rcs_sample_buffer_type type,
                           const rcs_send_audio_config& settings) {
    prepareSamples();
    std::lock_guard converting(converting_);
    uint64_t capture;
    {
        std::lock_guard lock(mutex_);
        if (!deliver_) {
            fail(RCS_ERROR_INVALID_STATE, "No sound is being sent");
        }
        capture = captures_;
    }
    if (std::exchange(mixing_, capture) != capture) { // The sound restarted: what was held belongs to the last.
        mixer_.reset();
    }
    const uint32_t percent[] = {settings.sample_buffer_audio_percent, settings.sample_buffer_microphone_percent};
    for (uint32_t source = 0; source < audio::Mixer::sources; ++source) {
        if (!percent[source]) {
            mixer_.leave(source);
        }
    }
    const uint32_t source = type == RCS_SAMPLE_BUFFER_MICROPHONE;
    if (!percent[source]) {
        return; // Left out.
    }
    converted_.clear();
    samples_[source]->convert(sampleBuffer, converted_);
    mixed_.clear();
    const uint64_t drops = mixer_.counters().drops;
    mixer_.push(source, converted_, float(percent[source]) / 100, monotonicNs(), mixed_);
    if (mixer_.counters().drops != drops) {
        log::info("audio", "The sample buffers' sounds: one ran ahead of the other and was set back (" +
                               std::to_string(mixer_.counters().droppedFrames) + " frames dropped in all)");
    }
    // In pieces of at most a second, as rcs_session_push_audio takes them.
    for (size_t at = 0; at < mixed_.size();) {
        const size_t frames = std::min((mixed_.size() - at) / audioChannels, size_t(audioRate));
        rcs_audio_frame frame = RCS_AUDIO_FRAME_INIT;
        frame.samples = mixed_.data() + at;
        frame.frames = uint32_t(frames);
        frame.channels = audioChannels;
        frame.sample_rate = audioRate;
        push(frame);
        at += frames * audioChannels;
    }
}

} // namespace rcsdk::session

namespace rcsdk::session {

void EncodedVideo::start(const agent::VideoStream& stream, Deliver deliver, std::function<uint64_t()> backlog) {
    std::lock_guard lock(mutex_);
    stream_ = stream;
    deliver_ = std::move(deliver);
    backlog_ = std::move(backlog);
    keyframeSeen_ = false;
    config_.clear();
}

void EncodedVideo::stop() {
    std::lock_guard lock(mutex_);
    deliver_ = {};
    backlog_ = {};
}

void EncodedVideo::setRate(uint32_t bitrate, bool budgeted) {
    std::lock_guard lock(mutex_);
    budget_.setRate(budgeted ? bitrate : 0);
}

void EncodedVideo::push(const rcs_encoded_packet& p) {
    if ((p.data_size && !p.data) || (p.config_size && !p.config)) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "data or config is NULL with a size");
    }
    if (const auto problem = temporalLayerProblem(p.temporal_id, p.discardable); !problem.empty()) {
        fail(RCS_ERROR_INVALID_ARGUMENT, problem);
    }
    EncodedPacket packet;
    packet.codec = p.codec;
    packet.temporalId = p.temporal_id;
    packet.discardable = p.discardable != 0;
    packet.width = p.width;
    packet.height = p.height;
    packet.keyframe = p.keyframe != 0;
    packet.timestampNs = p.timestamp_ns ? p.timestamp_ns : monotonicNs();
    packet.data.assign(p.data, p.data + p.data_size);
    packet.config.assign(p.config, p.config + p.config_size);
    std::lock_guard lock(mutex_);
    if (!deliver_) {
        fail(RCS_ERROR_INVALID_STATE, "No stream runs: one starts when a viewer connected (RCS_EVENT_KEYFRAME)");
    }
    const auto& s = stream_;
    if (packet.codec != s.codec || packet.width != s.width || packet.height != s.height) {
        fail(RCS_ERROR_INVALID_ARGUMENT, std::string("A packet of ") + codecName(packet.codec) + " " +
                                             std::to_string(packet.width) + "x" + std::to_string(packet.height) +
                                             "; the stream is " + codecName(s.codec) + " " + std::to_string(s.width) +
                                             "x" + std::to_string(s.height) + " (rcs_stream_info)");
    }
    if (!pictureOf(p.format, p.color, packet.picture) || packet.picture != s.picture) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "A packet whose format or color is not the stream's: " +
                                             pictureText(s.picture) + " (rcs_stream_info format and color)");
    }
    if (!keyframeSeen_ && !packet.keyframe) {
        fail(RCS_ERROR_INVALID_STATE, "A stream starts with a keyframe (RCS_EVENT_KEYFRAME)");
    }
    if (packet.temporalId >= s.temporalLayers) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "temporal_id " + std::to_string(packet.temporalId) +
                                             " is not below the stream's temporal layers (" +
                                             std::to_string(s.temporalLayers) + ", RCS_EXT_ENCODINGS scalability)");
    }
    fillConfig(packet, config_, s.framesPerSecond);
    if (auto problem = protocol::videoProblem(packet);
        !problem.empty() || !(problem = samplingProblem(packet)).empty()) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "The packet has " + problem);
    }
    if (packet.keyframe) {
        keyframeSeen_ = true;
        if (!hostCodec(packet.codec)) {
            config_ = packet.config;
        }
    }
    if (readTemporalLayer(packet) && !layerLogged_) {
        layerLogged_ = true;
        log::warning("encoder", "The host's packets say another temporal layer than their bitstream; the "
                                "bitstream's is sent");
    }
    packet.sequence = ++sequence_;
    budget_.spend(packet.data.size() + packet.config.size());
    deliver_(std::move(packet));
}

rcs_result EncodedVideo::ready() {
    std::lock_guard lock(mutex_);
    if (!deliver_) {
        fail(RCS_ERROR_INVALID_STATE, "No stream runs: one starts when a viewer connected (RCS_EVENT_KEYFRAME)");
    }
    // As the SDK's own capture holds back.
    return agent::behind(backlog_(), stream_.framesPerSecond) || budget_.exhausted() ? RCS_ERROR_QUEUE_FULL : RCS_OK;
}

} // namespace rcsdk::session
