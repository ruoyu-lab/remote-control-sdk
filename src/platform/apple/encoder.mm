// H.264 and HEVC encoding with VideoToolbox on macOS and iOS (none encodes
// AV1). Frames are submitted asynchronously; the output handler queues
// finished packets for receive(). The picture format chooses the profile
// (H.264 High or High 4:4:4 Predictive, HEVC Main, Main 10, Main 4:4:4 or Main
// 4:4:4 10) and the input's YCbCr layout, which capture delivers; CPU
// pictures and BGRA surfaces of another format than the default are converted
// to that layout first (VTPixelTransferSession): the low-latency encoder turns
// BGRA into 4:2:0 whatever the profile. The real-time encoder keeps long-term
// references, which make reference invalidation possible (invalidateAfter).
// Temporal layers: two where asked for (EncoderConfig::temporalLayers), every
// other picture in the upper one, which nothing refers to: the real-time
// encoders (BaseLayerFrameRateFraction 0.5; the fractions for three layers
// fail every picture on an M4 Pro) and the pipelined HEVC one
// (BaseLayerFrameRate); the pipelined H.264 encoder and the CPU one make none.
// HEVC says each picture's layer in its NAL units; H.264 does not (no SVC
// prefix units): a picture of the upper layer is one VideoToolbox says no
// other depends on (kCMSampleAttachmentKey_IsDependedOnByOthers).
#include "media/h264.hpp"
#include "media/nal.hpp"
#include "platform/apple/cf.hpp"
#include "platform/platform.hpp"
#include "util/log.hpp"

#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>

namespace rcsdk::platform {
namespace {

using apple::CFRef;

// The first of VideoToolbox's encoders for `type` with that acceleration.
apple::EncoderEntry findEncoder(CMVideoCodecType type, bool hardware) {
    for (auto& entry : apple::videoEncoders()) {
        if (entry.type == type && entry.hardware == hardware) {
            return entry;
        }
    }
    return {};
}

// Parameter set accessors share one signature across codecs.
using ParameterSetAccessor = OSStatus (*)(CMFormatDescriptionRef, size_t, const uint8_t**, size_t*, size_t*, int*);

class VideoToolboxEncoder final : public VideoEncoder {
  public:
    ~VideoToolboxEncoder() override {
        if (session_) {
            VTCompressionSessionCompleteFrames(session_.value, kCMTimeInvalid);
            VTCompressionSessionInvalidate(session_.value);
        }
        // CompleteFrames waits for the last output callback inside
        // VideoToolbox; the callback does all it does to this encoder holding
        // the lock, so taking it orders that before the members go.
        std::lock_guard lock(mutex_);
    }

    void open(const EncoderConfig& config) override {
        switch (config.codec) {
        case RCS_CODEC_H264:
            parameterSet_ = CMVideoFormatDescriptionGetH264ParameterSetAtIndex;
            break;
        case RCS_CODEC_HEVC:
            parameterSet_ = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex;
            break;
        default:
            fail(RCS_ERROR_UNSUPPORTED, std::string(codecName(config.codec)) + " encoding is not implemented on macOS");
        }
        type_ = apple::codecType(config.codec);
        config_ = config;
        bitrate_ = config.bitrate;
        name_ = codecName(config.codec);
        // VideoToolbox says which sizes it takes only by refusing a session.
        const std::string creating = std::string("Creating a ") + name_ + " encoder for " +
                                     std::to_string(config.width) + "x" + std::to_string(config.height) + " pictures";
        if (config.acceleration == RCS_ACCELERATION_SOFTWARE) {
            if (config.codec != RCS_CODEC_H264) { // The CPU HEVC encoder is not real time (see capabilities).
                fail(RCS_ERROR_UNSUPPORTED, std::string("No real-time software ") + name_ + " encoder");
            }
            apple::check(createSession(Mode::software), creating.c_str(), RCS_ERROR_CODEC);
            encoderName_ = findEncoder(type_, false).name;
            return;
        }
        hardwareEncoder_ = findEncoder(type_, true);
        if (!hardwareEncoder_.id) {
            fail(RCS_ERROR_UNSUPPORTED, std::string("No hardware ") + name_ + " encoder is available");
        }
        // The real-time encoder fails every frame beyond H.264's highest level
        // (Level 5.2: 2,073,600 macroblocks per second: 4096x2160 above 60 FPS,
        // 4096x2304 above 56). Both modes run on the hardware encoder; the
        // codec event and statistics report which one.
        const uint64_t macroblocks = uint64_t((config.width + 15) / 16) * ((config.height + 15) / 16);
        const bool withinLevel = config.codec != RCS_CODEC_H264 || macroblocks * config.framesPerSecond <= 2'073'600;
        // The real-time encoder takes H.264's 4:4:4 profile and encodes 4:2:0
        // all the same (an M4 Pro, macOS 26): only the pinned encoder does it.
        const bool realTime = withinLevel && !(config.codec == RCS_CODEC_H264 && chroma444(config.picture.format));
        if (!realTime || createSession(Mode::realTime) != noErr) {
            apple::check(createSession(Mode::pipelined), creating.c_str(), RCS_ERROR_CODEC);
        }
    }

    void submit(const VideoFrame& frame) override {
        {
            std::lock_guard lock(mutex_);
            if (!error_.empty()) { // From the output handler of an earlier frame.
                fail(RCS_ERROR_CODEC, error_);
            }
        }
        if (const uint32_t bitrate = pendingBitrate_.exchange(0)) {
            applyBitrate(bitrate);
        }
        CFRef<CVPixelBufferRef> copied;
        auto buffer = static_cast<CVPixelBufferRef>(frame.surface.get());
        if (!buffer) {
            buffer = upload(frame, copied);
        }
        if (config_.picture != Picture{} &&
            CVPixelBufferGetPixelFormatType(buffer) != apple::pixelFormatOf(config_.picture)) {
            buffer = transfer(buffer, copied);
        }
        if (CVPixelBufferGetWidth(buffer) != config_.width || CVPixelBufferGetHeight(buffer) != config_.height) {
            fail(RCS_ERROR_CODEC, "Frame size does not match the encoder");
        }
        Submitted submitted{{}, monotonicNs(), keyframeRequests_.load(), false, std::nullopt};
        submitted.header.width = frame.width;
        submitted.header.height = frame.height;
        submitted.header.sequence = frame.sequence;
        submitted.header.timestampNs = frame.timestampNs;
        CFRef<CFMutableArrayRef> acknowledged;
        {
            std::lock_guard lock(mutex_);
            // One forced frame at a time: a request answered by none (rate
            // control may drop the forced frame) forces the next one.
            submitted.forced = submitted.requests != answered_ && !forcing_;
            forcing_ |= submitted.forced;
            ++inFlight_;
            // A keyframe leaves out everything before it; otherwise the long-term
            // references the viewers have are acknowledged now, and this picture
            // refers to one of them only.
            if (submitted.forced) {
                refreshAfter_.reset();
            } else if (refreshAfter_) {
                submitted.refreshAfter = std::exchange(refreshAfter_, std::nullopt);
                acknowledged.reset(CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks));
                for (const auto& [sequence, token] : ltrTokens_) {
                    if (sequence > acknowledged_ && sequence <= *submitted.refreshAfter) {
                        const CFRef<CFNumberRef> number(CFNumberCreate(nullptr, kCFNumberSInt64Type, &token));
                        CFArrayAppendValue(acknowledged.value, number.value);
                        acknowledged_ = sequence;
                    }
                }
            }
        }
        CFRef<CFMutableDictionaryRef> options;
        if (submitted.forced) {
            options = apple::dictionary();
            CFDictionarySetValue(options.value, kVTEncodeFrameOptionKey_ForceKeyFrame, kCFBooleanTrue);
        } else if (submitted.refreshAfter) {
            options = apple::dictionary();
            CFDictionarySetValue(options.value, kVTEncodeFrameOptionKey_AcknowledgedLTRTokens, acknowledged.value);
            CFDictionarySetValue(options.value, kVTEncodeFrameOptionKey_ForceLTRRefresh, kCFBooleanTrue);
        }
        const OSStatus status = VTCompressionSessionEncodeFrameWithOutputHandler(
            session_.value, buffer, CMTimeMake(int64_t(frame.timestampNs), 1'000'000'000),
            CMTimeMake(1, int32_t(config_.framesPerSecond)), options.value, nullptr,
            ^(OSStatus result, VTEncodeInfoFlags flags, CMSampleBufferRef sample) {
                finished(submitted, result, flags, sample);
            });
        if (status != noErr) {
            {
                std::lock_guard lock(mutex_);
                --inFlight_;
                forcing_ &= !submitted.forced;
            }
            apple::check(status, "Encode", RCS_ERROR_CODEC);
        }
        if (mode_ == Mode::software) {
            // VideoToolbox's CPU encoders hold ~13 frames for lookahead despite
            // MaxFrameDelayCount 0; completing each frame keeps them real time.
            VTCompressionSessionCompleteFrames(session_.value, CMTimeMake(int64_t(frame.timestampNs), 1'000'000'000));
        }
        adapt(monotonicNs() - submitted.startedNs);
    }

    bool receive(EncodedPacket& packet, std::chrono::milliseconds timeout) override {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, timeout, [&] { return !ready_.empty() || !error_.empty(); })) {
            return false;
        }
        if (!error_.empty()) {
            fail(RCS_ERROR_CODEC, error_);
        }
        packet = std::move(ready_.front());
        ready_.pop_front();
        --inFlight_;
        return true;
    }

    size_t inFlight() const noexcept override {
        std::lock_guard lock(mutex_);
        return inFlight_;
    }
    void requestKeyframe() noexcept override { ++keyframeRequests_; }
    // The next picture refers only to a long-term reference from up to
    // `sequence`, one of those VideoToolbox marked (and reported with a token)
    // that it is now told the viewers have. VideoToolbox makes it a keyframe
    // where it holds none. Acknowledged references stay usable: one already
    // acknowledged from after `sequence`, or an encoder without long-term
    // references (the pipelined and software ones), takes a keyframe.
    void invalidateAfter(uint64_t sequence) noexcept override {
        std::lock_guard lock(mutex_);
        if (!ltr_ || sequence < acknowledged_) {
            ++keyframeRequests_;
            return;
        }
        refreshAfter_ = std::min(refreshAfter_.value_or(sequence), sequence);
    }
    bool refInvalidation() const noexcept override {
        std::lock_guard lock(mutex_);
        return ltr_;
    }
    void setBitrate(uint32_t bitsPerSecond) noexcept override { pendingBitrate_ = bitsPerSecond; }
    uint32_t bitrate() const noexcept override { return bitrate_; }
    bool lowLatency() const noexcept override { return mode_ == Mode::realTime; }
    uint32_t temporalLayers() const noexcept override { return layered_ ? 2 : 1; }
    std::string name() const override {
        // The real-time encoder is VideoToolbox's low-latency mode of the same hardware.
        return mode_ == Mode::software   ? encoderName_
               : mode_ == Mode::realTime ? hardwareEncoder_.name + ", low latency"
                                         : hardwareEncoder_.name;
    }
    rcs_backend backend() const noexcept override { return RCS_BACKEND_VIDEOTOOLBOX; }
    void onOutputReady(std::function<void()> callback) override { outputReady_ = std::move(callback); }

  private:
    // realTime: low-latency rate control selects Apple's real-time encoder on
    // the hardware encoder (4K HEVC on an M4 Pro: 18 ms instead of 33 ms per
    // frame). It cannot be combined with a pinned EncoderID.
    // pipelined: the pinned hardware encoder, several frames in flight.
    // software: VideoToolbox's CPU encoders only.
    enum class Mode { realTime, pipelined, software };

    // What the output handler needs to know about its frame.
    struct Submitted {
        EncodedPacket header;
        uint64_t startedNs = 0;
        uint64_t requests = 0; // Keyframe requests made before it was submitted.
        bool forced = false;   // It was asked to be a keyframe.
        std::optional<uint64_t> refreshAfter; // It was asked to refer to nothing after this.
    };

    OSStatus createSession(Mode mode) {
        if (session_) {
            VTCompressionSessionCompleteFrames(session_.value, kCMTimeInvalid);
            VTCompressionSessionInvalidate(session_.value);
            session_.reset();
        }
        auto specification = apple::dictionary();
        switch (mode) {
        case Mode::realTime:
            CFDictionarySetValue(specification.value, kVTVideoEncoderSpecification_EnableLowLatencyRateControl,
                                 kCFBooleanTrue);
            break;
        case Mode::pipelined:
            CFDictionarySetValue(specification.value, kVTVideoEncoderSpecification_EncoderID,
                                 hardwareEncoder_.id.get());
            if (@available(macOS 10.9, iOS 17.4, *)) { // Before, iOS takes the hardware encoder's id alone.
                CFDictionarySetValue(specification.value,
                                     kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder,
                                     kCFBooleanTrue);
            }
            break;
        case Mode::software:
            if (@available(macOS 10.9, iOS 17.4, *)) {
                CFDictionarySetValue(specification.value,
                                     kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                                     kCFBooleanFalse);
            } else {
                return kVTVideoEncoderNotAvailableNowErr; // iOS before 17.4 cannot keep an encoder off the hardware.
            }
            break;
        }
        // Screen capture delivers the stream's YCbCr layout; declaring it lets
        // VideoToolbox skip a pixel transfer.
        auto attributes = apple::surfaceAttributes(config_.width, config_.height, apple::pixelFormatOf(config_.picture));
        const OSStatus status = VTCompressionSessionCreate(nullptr, int32_t(config_.width), int32_t(config_.height),
                                                           type_, specification.value, attributes.value, nullptr,
                                                           nullptr, nullptr, session_.out());
        if (status != noErr) {
            return status;
        }
        // The real-time encoder cannot require hardware. Where it reports
        // running on the CPU it is unavailable, not a substitute. (iOS says
        // from 17.4 on; its real-time encoders run on the hardware.)
        if (@available(macOS 10.9, iOS 17.4, *)) {
            CFRef<CFBooleanRef> usingHardware;
            if (mode == Mode::realTime &&
                VTSessionCopyProperty(session_.value, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
                                      nullptr, usingHardware.out()) == noErr &&
                usingHardware && !CFBooleanGetValue(usingHardware.value)) {
                VTCompressionSessionInvalidate(session_.value);
                session_.reset();
                return kVTCouldNotFindVideoEncoderErr;
            }
        }
        mode_ = mode;
        {
            // Long-term references, where the encoder keeps them (the
            // real-time one; the pipelined one refuses them).
            std::lock_guard lock(mutex_);
            ltr_ = VTSessionSetProperty(session_.value, kVTCompressionPropertyKey_EnableLTR, kCFBooleanTrue) == noErr;
            ltrTokens_.clear();
            acknowledged_ = 0;
            refreshAfter_.reset();
        }
        // Two temporal layers where asked for and the encoder makes them.
        layered_ = config_.temporalLayers > 1 &&
                   (mode == Mode::realTime
                        ? VTSessionSetProperty(session_.value, kVTCompressionPropertyKey_BaseLayerFrameRateFraction,
                                               (__bridge CFNumberRef) @0.5) == noErr
                        : mode == Mode::pipelined && config_.codec == RCS_CODEC_HEVC &&
                              VTSessionSetProperty(session_.value, kVTCompressionPropertyKey_BaseLayerFrameRate,
                                                   (__bridge CFNumberRef) @(config_.framesPerSecond / 2.0)) == noErr);
        auto properties = apple::dictionary();
        CFDictionarySetValue(properties.value, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
        CFDictionarySetValue(properties.value, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
        CFDictionarySetValue(properties.value, kVTCompressionPropertyKey_ProfileLevel, profile());
        apple::setNumber(properties.value, kVTCompressionPropertyKey_AverageBitRate, bitrate_);
        apple::setNumber(properties.value, kVTCompressionPropertyKey_ExpectedFrameRate, config_.framesPerSecond);
        // Keyframes only when the session asks (rcs_encoder_config.keyframe_interval_ms).
        // The hardware encoders' default is no limit; the software encoder makes one
        // every 30 frames unless told a longer interval (it takes 0 as its default).
        apple::setNumber(properties.value, kVTCompressionPropertyKey_MaxKeyFrameInterval, INT32_MAX);
        apple::setNumber(properties.value, kVTCompressionPropertyKey_MaxFrameDelayCount, 0);
        // The stream's colour in the bitstream's VUI: primaries, transfer and
        // matrix here, the range from the input's pixel format.
        const auto colour = apple::colorKeysOf(config_.picture);
        CFDictionarySetValue(properties.value, kVTCompressionPropertyKey_ColorPrimaries, colour.primaries);
        CFDictionarySetValue(properties.value, kVTCompressionPropertyKey_TransferFunction, colour.transfer);
        CFDictionarySetValue(properties.value, kVTCompressionPropertyKey_YCbCrMatrix, colour.matrix);
        apple::check(VTSessionSetProperties(session_.value, properties.value), "Encoder configuration", RCS_ERROR_CODEC);
        // The whole quantizer range, as on the other systems: VideoToolbox
        // stops near 45 of 51 by default, so on busy pictures at a low
        // bitrate it cannot make a picture small enough, and pacing then
        // holds capture back (1080p at 2 Mbit/s: 3 FPS and 495 kbit a
        // picture; with 51: 10 FPS and 142 kbit; 640x360 at 0.6 Mbit/s: 6 ->
        // 21 FPS). At ordinary bitrates it never comes near either.
        const int32_t mostQuantizer = 51;
        if (VTSessionSetProperty(session_.value, kVTCompressionPropertyKey_MaxAllowedFrameQP,
                                 CFRef<CFNumberRef>(CFNumberCreate(nullptr, kCFNumberSInt32Type, &mostQuantizer)).value) !=
            noErr) {
            log::debug("encoder", "The encoder keeps its own quantizer limit");
        }
        // rcs_encoder_config.speed, where the encoder takes it (BALANCED: its own default).
        if (config_.speed != RCS_ENCODER_BALANCED &&
            VTSessionSetProperty(session_.value, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality,
                                 config_.speed == RCS_ENCODER_FASTEST ? kCFBooleanTrue : kCFBooleanFalse) != noErr) {
            log::debug("encoder", "The encoder keeps its own trade of speed and quality");
        }
        apple::check(VTCompressionSessionPrepareToEncodeFrames(session_.value), "Encoder preparation", RCS_ERROR_CODEC);
        return noErr;
    }

    // A new target from the next frame on; one VideoToolbox refuses leaves
    // the previous in place, which bitrate() reports.
    void applyBitrate(uint32_t bitrate) {
        const auto number = CFRef<CFNumberRef>(CFNumberCreate(nullptr, kCFNumberSInt32Type, &bitrate));
        const OSStatus status =
            VTSessionSetProperty(session_.value, kVTCompressionPropertyKey_AverageBitRate, number.value);
        if (status == noErr) {
            bitrate_ = bitrate; // Also for a session the encoder may switch to.
        } else if (!bitrateRefused_) {
            bitrateRefused_ = true;
            log::warning("encoder", "The encoder refused a bitrate of " + std::to_string(bitrate) +
                                        " bit/s (OSStatus " + std::to_string(status) + "); it stays at " +
                                        std::to_string(bitrate_.load()));
        }
    }

    // The real-time encoder works inside the submit call, one frame at a time.
    // When that does not fit the frame interval (4K60 on an M4 Pro manages
    // 52 FPS), switch once to the pipelined encoder: ~15 ms more latency per
    // frame, but the full frame rate. The new session starts with a keyframe.
    void adapt(uint64_t blockedNs) {
        constexpr int warmup = 5, window = 30;
        if (mode_ != Mode::realTime || adapted_ || ++measured_ <= warmup) {
            return;
        }
        blockedNs_ += blockedNs;
        if (measured_ < warmup + window) {
            return;
        }
        const uint64_t interval = 1'000'000'000ull / config_.framesPerSecond;
        adapted_ = true; // Decided either way.
        if (blockedNs_ / window * 4 > interval * 3) {
            apple::check(createSession(Mode::pipelined), "Encoder creation", RCS_ERROR_CODEC);
        }
    }

    // The profile of the stream's picture format (encoderFormats() in
    // system.mm lists the formats whose profiles the encoder offers).
    CFStringRef profile() const {
        if (config_.codec == RCS_CODEC_H264) {
            return chroma444(config_.picture.format) ? CFSTR("H264_High444Predictive_AutoLevel")
                                                     : kVTProfileLevel_H264_High_AutoLevel;
        }
        const bool wide = bitDepth(config_.picture.format) > 8;
        if (chroma444(config_.picture.format)) {
            return wide ? CFSTR("HEVC_Main44410_AutoLevel") : CFSTR("HEVC_Main444_AutoLevel");
        }
        return wide ? kVTProfileLevel_HEVC_Main10_AutoLevel : kVTProfileLevel_HEVC_Main_AutoLevel;
    }

    // A picture in the stream's YCbCr layout, of its matrix and range: what
    // VideoToolbox's own conversion would not make (see the top).
    CVPixelBufferRef transfer(CVPixelBufferRef source, CFRef<CVPixelBufferRef>& owner) {
        if (!transfer_) {
            apple::check(VTPixelTransferSessionCreate(nullptr, transfer_.out()), "Encoder input conversion",
                       RCS_ERROR_CODEC);
            const auto colour = apple::colorKeysOf(config_.picture);
            VTSessionSetProperty(transfer_.value, kVTPixelTransferPropertyKey_DestinationColorPrimaries,
                                 colour.primaries);
            VTSessionSetProperty(transfer_.value, kVTPixelTransferPropertyKey_DestinationTransferFunction,
                                 colour.transfer);
            VTSessionSetProperty(transfer_.value, kVTPixelTransferPropertyKey_DestinationYCbCrMatrix, colour.matrix);
        }
        CFRef<CVPixelBufferRef> converted;
        auto attributes = apple::surfaceAttributes(config_.width, config_.height, apple::pixelFormatOf(config_.picture));
        if (CVPixelBufferCreate(nullptr, config_.width, config_.height, apple::pixelFormatOf(config_.picture),
                                attributes.value, converted.out()) != kCVReturnSuccess) {
            fail(RCS_ERROR_CODEC, "Could not prepare the encoder input");
        }
        apple::check(VTPixelTransferSessionTransferImage(transfer_.value, source, converted.value),
                   "Encoder input conversion", RCS_ERROR_CODEC);
        owner = std::move(converted);
        return owner.value;
    }

    // CPU BGRA frames (test sources) go into a BGRA surface; VideoToolbox converts it.
    CVPixelBufferRef upload(const VideoFrame& frame, CFRef<CVPixelBufferRef>& owner) {
        auto attributes = apple::surfaceAttributes(frame.width, frame.height);
        if (!frame.pixels || CVPixelBufferCreate(nullptr, frame.width, frame.height, kCVPixelFormatType_32BGRA,
                                                 attributes.value, owner.out()) != kCVReturnSuccess) {
            fail(RCS_ERROR_CODEC, "Could not prepare the encoder input");
        }
        CVPixelBufferRef buffer = owner.value;
        CVPixelBufferLockBaseAddress(buffer, 0);
        auto* destination = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
        const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
        for (uint32_t y = 0; y < frame.height; ++y) {
            std::memcpy(destination + y * stride, frame.pixels->data() + size_t(y) * frame.stride, frame.width * 4);
        }
        CVPixelBufferUnlockBaseAddress(buffer, 0);
        return buffer;
    }

    // Runs on a VideoToolbox thread, or inside the submit call (real-time
    // encoder). Nothing may escape it: a failure waits for the next submit()
    // or receive().
    void finished(Submitted submitted, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample) noexcept {
        // Rate control may skip a frame (e.g. right after the bitrate
        // dropped): nothing to send, not an error.
        const bool dropped = status == noErr && (!sample || (flags & kVTEncodeInfo_FrameDropped));
        std::unique_lock lock(mutex_, std::defer_lock); // Held to the end (see the destructor).
        try {
            EncodedPacket& packet = submitted.header;
            std::string error;
            std::optional<int64_t> token;
            if (status != noErr) {
                error = std::string(name_) + " encoder output failed (OSStatus " + std::to_string(status) + ")";
            } else if (!dropped) {
                packet.encodeNs = monotonicNs() - submitted.startedNs;
                error = describe(sample, packet, token);
            }
            lock.lock();
            forcing_ &= !submitted.forced; // A forced frame that came out as no keyframe arms the next.
            if (dropped) {
                --inFlight_;
                if (submitted.refreshAfter && !refreshAfter_) { // The refresh moves to the next picture.
                    refreshAfter_ = submitted.refreshAfter;
                }
            } else if (!error.empty()) {
                if (error_.empty()) {
                    error_ = std::move(error);
                }
            } else {
                if (packet.keyframe) {
                    answered_ = std::max(answered_, submitted.requests);
                    ltrTokens_.clear(); // Nothing before a keyframe is referred to again.
                } else if (submitted.refreshAfter) {
                    packet.recoversAfter = submitted.refreshAfter;
                }
                if (token) {
                    ltrTokens_.emplace_back(packet.sequence, *token);
                    if (ltrTokens_.size() > maxLtrTokens) {
                        ltrTokens_.pop_front();
                    }
                }
                ready_.push_back(std::move(packet));
            }
        } catch (const std::exception& failure) { // An SPS it cannot rewrite; memory.
            if (!lock.owns_lock()) {
                lock.lock();
            }
            forcing_ &= !submitted.forced;
            if (error_.empty()) {
                error_ = std::string(name_) + " encoder output: " + failure.what();
            }
        }
        changed_.notify_all();
        if (outputReady_ && !dropped) {
            outputReady_();
        }
    }

    // The packet of `sample`, and the token of a long-term reference it is.
    std::string describe(CMSampleBufferRef sample, EncodedPacket& packet, std::optional<int64_t>& token) {
        CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
        CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
        if (!format || !block) {
            return "Encoder produced no bitstream";
        }
        int nalLength = 0;
        size_t count = 0;
        if (parameterSet_(format, 0, nullptr, nullptr, &count, &nalLength) != noErr || count < 2 || nalLength != 4) {
            return "Encoder produced unexpected parameter sets";
        }
        if (format != lastFormat_.value) { // Rebuild config only when the stream changes.
            configBytes_.clear();
            for (size_t i = 0; i < count; ++i) {
                const uint8_t* bytes = nullptr;
                size_t size = 0;
                parameterSet_(format, i, &bytes, &size, nullptr, nullptr);
                const std::span<const uint8_t> unit(bytes, size);
                if (config_.codec == RCS_CODEC_H264 && nal::h264::type(unit) == nal::h264::sps) {
                    // AllowFrameReordering is off; declare it so receivers do not buffer.
                    nal::appendUnit(configBytes_, h264::noReorderingSps(unit));
                } else {
                    nal::appendUnit(configBytes_, unit);
                }
            }
            CFRetain(format);
            lastFormat_.reset(format);
        }
        packet.config = configBytes_;
        packet.data.resize(CMBlockBufferGetDataLength(block));
        if (packet.data.empty() ||
            CMBlockBufferCopyDataBytes(block, 0, packet.data.size(), packet.data.data()) != kCMBlockBufferNoErr) {
            return "Could not copy the bitstream";
        }
        packet.codec = config_.codec;
        packet.keyframe = true;
        NSArray* attachments = (__bridge NSArray*)CMSampleBufferGetSampleAttachmentsArray(sample, false);
        if (NSNumber* notSync = attachments.firstObject[(__bridge NSString*)kCMSampleAttachmentKey_NotSync]) {
            packet.keyframe = !notSync.boolValue;
        }
        const auto ltrKey = (__bridge NSString*)kVTSampleAttachmentKey_RequireLTRAcknowledgementToken;
        if (NSNumber* ltr = attachments.firstObject[ltrKey]) {
            token = ltr.longLongValue;
        }
        // Two layers: the upper one is what no other picture depends on.
        NSNumber* depended = attachments.firstObject[(__bridge NSString*)kCMSampleAttachmentKey_IsDependedOnByOthers];
        packet.discardable = depended && !depended.boolValue;
        packet.temporalId = layered_ && packet.discardable && !packet.keyframe ? 1 : 0;
        return {};
    }

    EncoderConfig config_;
    CMVideoCodecType type_ = 0;
    const char* name_ = "";
    ParameterSetAccessor parameterSet_ = nullptr;
    apple::EncoderEntry hardwareEncoder_;
    std::atomic<uint32_t> pendingBitrate_{0};
    std::atomic<uint32_t> bitrate_{0};
    bool bitrateRefused_ = false;
    std::string encoderName_;
    CFRef<VTCompressionSessionRef> session_;
    CFRef<VTPixelTransferSessionRef> transfer_; // BGRA in, the stream's layout out.
    Mode mode_ = Mode::realTime;
    std::atomic<bool> layered_{false}; // It makes two temporal layers.
    bool adapted_ = false;
    int measured_ = 0;
    uint64_t blockedNs_ = 0;
    std::atomic<uint64_t> keyframeRequests_{0};
    std::function<void()> outputReady_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<EncodedPacket> ready_;
    size_t inFlight_ = 0;
    std::string error_;
    uint64_t answered_ = 0; // Keyframe requests a keyframe has answered.
    bool forcing_ = false;  // A forced frame is on its way.
    // Long-term references: whether the session keeps them, the tokens of
    // those since the last keyframe (sequence, token; oldest first, the
    // latest maxLtrTokens), the latest picture acknowledged, and a refresh
    // the next picture makes (invalidateAfter).
    static constexpr size_t maxLtrTokens = 64;
    bool ltr_ = false;
    std::deque<std::pair<uint64_t, int64_t>> ltrTokens_;
    uint64_t acknowledged_ = 0;
    std::optional<uint64_t> refreshAfter_;

    // Touched only on the VideoToolbox output thread.
    CFRef<CMFormatDescriptionRef> lastFormat_;
    std::vector<uint8_t> configBytes_;
};

} // namespace

std::unique_ptr<VideoEncoder> createVideoEncoder(const EncoderConfig&) {
    return std::make_unique<VideoToolboxEncoder>();
}

} // namespace rcsdk::platform
