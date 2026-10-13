#include "agent/host_encoder.hpp"

#include "media/encoder_output.hpp"
#include "media/gpu_frame.hpp"
#include "media/layers.hpp"
#include "media/sampling.hpp"
#include "protocol/messages.hpp"
#include "util/log.hpp"

#include <algorithm>

namespace rcsdk::agent {
namespace {

// "The host's encoder X ... (result)".
std::string what(const rcs_host_encoder_config& config, const char* happened, rcs_result result) {
    return std::string("The host's encoder ") + config.name + " " + happened + " (" + rcs_result_message(result) +
           (codecFailure(result) != result ? ", " + std::to_string(result) : std::string()) + ")";
}

} // namespace

HostEncoder::~HostEncoder() {
    bool opened;
    {
        std::lock_guard lock(mutex_);
        opened = open_;
    }
    if (opened) {
        config_.close(stream_, encoding_, config_.user_data);
        std::lock_guard lock(mutex_);
        open_ = false; // Later pushes are refused.
    }
}

void HostEncoder::open(const platform::EncoderConfig& config) {
    opened_ = config;
    bitrate_ = config.bitrate;
    rcs_host_encoder_settings settings{};
    settings.struct_size = sizeof(settings);
    settings.stream = stream_;
    settings.encoding = encoding_;
    settings.codec = config.codec;
    settings.format = config.picture.format;
    settings.color = colorOf(config.picture);
    settings.width = config.width;
    settings.height = config.height;
    settings.frames_per_second = config.framesPerSecond;
    settings.bitrate_bps = config.bitrate;
    settings.speed = config.speed;
    settings.temporal_layers = temporalLayers();
    settings.d3d11_device = config_.cpu_images ? nullptr : config.device.get();
    settings.output = &output_;
    {
        std::lock_guard lock(mutex_);
        open_ = true; // It may push from open on.
    }
    if (const rcs_result result = config_.open(&settings, config_.user_data); result != RCS_OK) {
        {
            std::lock_guard lock(mutex_);
            open_ = false;
        }
        fail(codecFailure(result), what(config_, "did not open", result));
    }
}

void HostEncoder::submit(const VideoFrame& frame) {
    rcs_host_encoder_frame picture{};
    picture.struct_size = sizeof(picture);
    picture.stream = stream_;
    picture.encoding = encoding_;
    picture.sequence = frame.sequence;
    picture.timestamp_ns = frame.timestampNs;
    picture.keyframe = keyframe_.exchange(false) ? 1u : 0u;
    {
        std::lock_guard lock(mutex_);
        pending_.push_back(frame.sequence);
        lastSubmitted_ = frame.sequence;
    }
    rcs_result result = RCS_OK;
    if (frame.surface && !config_.cpu_images) {
        auto* handle = new rcs_gpu_frame_impl;
        handle->frame = frame;
        handle->info.struct_size = sizeof(handle->info);
        platform::describeGpuFrame(frame, handle->info);
        picture.gpu_frame = handle;
        result = config_.encode(&picture, config_.user_data);
        rcs_gpu_frame_release(handle); // The host's retains keep it.
    } else if (frame.pixels) {
        rcs_frame view{};
        view.struct_size = sizeof(view);
        view.stream = stream_;
        view.width = frame.width;
        view.height = frame.height;
        view.stride_bytes = frame.stride;
        view.format = RCS_PIXEL_BGRA8;
        view.sequence = frame.sequence;
        view.timestamp_ns = frame.timestampNs;
        view.data = frame.pixels->data();
        view.data_size = frame.pixels->size();
        picture.cpu_frame = &view;
        result = config_.encode(&picture, config_.user_data);
    } else {
        platform::withCpuImage(frame, [&](const rcs_frame& view) {
            picture.cpu_frame = &view;
            result = config_.encode(&picture, config_.user_data);
        });
    }
    if (result != RCS_OK) {
        fail(codecFailure(result), what(config_, "failed to encode", result));
    }
}

void HostEncoder::push(const rcs_encoded_packet& p) {
    if ((p.data_size && !p.data) || (p.config_size && !p.config)) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "data or config is NULL with a size");
    }
    if (const auto problem = temporalLayerProblem(p.temporal_id, p.discardable); !problem.empty()) {
        fail(RCS_ERROR_INVALID_ARGUMENT, problem);
    }
    if (p.temporal_id >= temporalLayers()) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "temporal_id " + std::to_string(p.temporal_id) +
                                             " is not below the temporal layers it was opened for (" +
                                             std::to_string(temporalLayers()) + ")");
    }
    EncodedPacket packet;
    packet.stream = stream_;
    packet.encoding = encoding_;
    packet.temporalId = p.temporal_id;
    packet.discardable = p.discardable != 0;
    packet.codec = p.codec;
    const bool described = pictureOf(p.format, p.color, packet.picture);
    packet.width = p.width;
    packet.height = p.height;
    packet.keyframe = p.keyframe != 0;
    packet.sequence = p.sequence;
    packet.timestampNs = p.timestamp_ns;
    packet.data.assign(p.data, p.data + p.data_size);
    packet.config.assign(p.config, p.config + p.config_size);
    std::function<void()> ready;
    {
        std::lock_guard lock(mutex_);
        if (!open_) {
            fail(RCS_ERROR_INVALID_STATE, "The encoder is closed");
        }
        if (packet.codec != opened_.codec || packet.width != opened_.width || packet.height != opened_.height) {
            fail(RCS_ERROR_INVALID_ARGUMENT,
                 std::string("A packet of ") + codecName(packet.codec) + " " + std::to_string(packet.width) + "x" +
                     std::to_string(packet.height) + ", the encoder was opened for " + codecName(opened_.codec) + " " +
                     std::to_string(opened_.width) + "x" + std::to_string(opened_.height));
        }
        if (!described || packet.picture != opened_.picture) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "A packet whose format or color is not the one the encoder was opened "
                                             "for: " +
                                                 pictureText(opened_.picture));
        }
        if (packet.sequence > lastSubmitted_) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "sequence " + std::to_string(packet.sequence) +
                                                 " belongs to no picture given to encode yet");
        }
        fillConfig(packet, streamConfig_, opened_.framesPerSecond);
        if (auto problem = protocol::videoProblem(packet);
            !problem.empty() || !(problem = samplingProblem(packet)).empty()) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "The packet has " + problem);
        }
        if (packet.keyframe && !hostCodec(packet.codec)) {
            streamConfig_ = packet.config;
        }
        while (!pending_.empty() && pending_.front() <= packet.sequence) {
            pending_.pop_front(); // This picture and the ones it skipped are done.
        }
        packets_.push_back(std::move(packet));
        ready = outputReady_;
    }
    ready_.notify_all();
    if (ready) {
        ready();
    }
}

bool HostEncoder::receive(EncodedPacket& packet, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    if (!ready_.wait_for(lock, timeout, [&] { return !packets_.empty(); })) {
        return false;
    }
    packet = std::move(packets_.front());
    packets_.pop_front();
    return true;
}

size_t HostEncoder::inFlight() const noexcept {
    std::lock_guard lock(mutex_);
    return pending_.size() + packets_.size();
}

void HostEncoder::setBitrate(uint32_t bitsPerSecond) noexcept {
    const rcs_result result =
        config_.set_bitrate ? config_.set_bitrate(stream_, encoding_, bitsPerSecond, config_.user_data)
                            : RCS_ERROR_UNSUPPORTED;
    if (result == RCS_OK) {
        bitrate_ = bitsPerSecond;
    } else if (!refused_) {
        refused_ = true;
        log::warning("encoder", what(config_, ("kept its bitrate instead of " + std::to_string(bitsPerSecond / 1000) +
                                               " kbit/s").c_str(),
                                     result));
    }
}

void HostEncoder::onOutputReady(std::function<void()> callback) {
    std::lock_guard lock(mutex_);
    outputReady_ = std::move(callback);
}

} // namespace rcsdk::agent
