// The VideoToolbox encoders and decoders this SDK uses, and the picture
// formats each takes (macOS and iOS).
#include "media/picture_size.hpp"
#include "platform/apple/cf.hpp"
#include "platform/apple/codecs.hpp"
#include "platform/decode_probes.hpp"
#include "platform/platform.hpp"

#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

namespace rcsdk::platform::apple {
namespace {

// Codecs this SDK encodes and decodes with VideoToolbox. Apple silicon decodes
// VP9 and, from the M3 and A17 Pro on, AV1 in hardware; none encodes them,
// and VideoToolbox decodes them on no CPU.
constexpr rcs_codec encodedCodecs[] = {RCS_CODEC_H264, RCS_CODEC_HEVC};
constexpr rcs_codec decodedCodecs[] = {RCS_CODEC_H264, RCS_CODEC_HEVC, RCS_CODEC_VP9, RCS_CODEC_AV1};
constexpr bool softwareDecodes(rcs_codec codec) {
    return codec == RCS_CODEC_H264 || codec == RCS_CODEC_HEVC;
}

std::string text(NSString* value) {
    return value ? std::string(value.UTF8String) : std::string();
}

constexpr uint32_t bit(rcs_picture_format format) {
    return 1u << format;
}

// What the real-time encoder for `type` does where it runs on the hardware
// (encoder.mm): whether it keeps long-term references, which make reference
// invalidation possible (the pipelined hardware encoder refuses them), and
// whether it makes two temporal layers.
struct RealTime {
    bool longTermReferences = false, layers = false;
};
RealTime realTime(CMVideoCodecType type) {
    NSDictionary* specification =
        @{(__bridge NSString*)kVTVideoEncoderSpecification_EnableLowLatencyRateControl : @YES};
    CFRef<VTCompressionSessionRef> session;
    if (VTCompressionSessionCreate(nullptr, 640, 360, type, (__bridge CFDictionaryRef)specification, nullptr, nullptr,
                                   nullptr, nullptr, session.out()) != noErr) {
        return {};
    }
    // As encoder.mm: one that says it runs on the CPU is not used (iOS says
    // from 17.4 on: before, the key is not there to read).
    bool software = false;
    if (@available(macOS 10.9, iOS 17.4, *)) {
        CFRef<CFBooleanRef> hardware;
        software = VTSessionCopyProperty(session.value, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
                                         nullptr, hardware.out()) == noErr &&
                   hardware && !CFBooleanGetValue(hardware.value);
    }
    RealTime features;
    features.longTermReferences =
        !software && VTSessionSetProperty(session.value, kVTCompressionPropertyKey_EnableLTR, kCFBooleanTrue) == noErr;
    features.layers =
        !software && VTSessionSetProperty(session.value, kVTCompressionPropertyKey_BaseLayerFrameRateFraction,
                                          (__bridge CFNumberRef) @0.5) == noErr;
    VTCompressionSessionInvalidate(session.value);
    return features;
}

// The picture formats an encoder takes, from the profiles VideoToolbox lists
// for it: 4:4:4 where H.264's High 4:4:4 Predictive or HEVC's Main 4:4:4 is
// among them, 10 bits where HEVC's Main 10 or Main 4:4:4 10 is (an M4 Pro's
// hardware encoders; its software H.264 encoder has neither).
uint32_t encoderFormats(const EncoderEntry& entry) {
    NSDictionary* specification =
        @{(__bridge NSString*)kVTVideoEncoderSpecification_EncoderID : (__bridge id)entry.id.get()};
    CFDictionaryRef supported = nullptr;
    if (VTCopySupportedPropertyDictionaryForEncoder(1920, 1080, entry.type, (__bridge CFDictionaryRef)specification,
                                                    nullptr, &supported) != noErr) {
        return defaultPictureFormats;
    }
    NSDictionary* properties = (__bridge NSDictionary*)supported;
    NSArray* profiles = properties[(__bridge NSString*)kVTCompressionPropertyKey_ProfileLevel]
                                  [(__bridge NSString*)kVTPropertySupportedValueListKey];
    const auto offers = [&](NSString* profile) { return [profiles containsObject:profile]; };
    uint32_t formats = defaultPictureFormats;
    if (offers(@"H264_High444Predictive_AutoLevel") || offers(@"HEVC_Main444_AutoLevel")) {
        formats |= bit(RCS_PICTURE_444_8);
    }
    if (offers(@"HEVC_Main10_AutoLevel")) {
        formats |= bit(RCS_PICTURE_420_10);
    }
    if (offers(@"HEVC_Main44410_AutoLevel")) {
        formats |= bit(RCS_PICTURE_444_10);
    }
    CFRelease(supported);
    return formats;
}

// Whether VideoToolbox's decoder of `codec` with that acceleration decodes a
// keyframe of `format` (decode_probes.hpp): VideoToolbox says only which
// codecs it decodes, not which profiles.
bool decodes(rcs_codec codec, bool hardware, rcs_picture_format format, std::span<const uint8_t> config,
             std::span<const uint8_t> data) {
    try {
        DecoderConfig settings;
        settings.codec = codec;
        settings.acceleration = hardware ? RCS_ACCELERATION_HARDWARE : RCS_ACCELERATION_SOFTWARE;
        settings.picture.format = format;
        auto decoder = createVideoDecoder(settings);
        decoder->open(settings);
        EncodedPacket packet;
        packet.codec = codec;
        packet.picture.format = format;
        packet.width = probes::width;
        packet.height = probes::height;
        packet.keyframe = true;
        packet.config.assign(config.begin(), config.end());
        packet.data.assign(data.begin(), data.end());
        VideoFrame frame;
        static const std::atomic<bool> never{false};
        return decoder->decode(packet, frame, never) && frame.width == probes::width;
    } catch (const Failure&) {
        return false;
    }
}

// The picture formats a decoder takes: 4:2:0 8-bit, and those of the probes
// it decodes.
uint32_t decoderFormats(rcs_codec codec, bool hardware) {
    uint32_t formats = defaultPictureFormats;
    for (const auto& probe : probes::all) {
        if (probe.codec == codec && decodes(codec, hardware, probe.format, probe.config, probe.data)) {
            formats |= bit(probe.format);
        }
    }
    return formats;
}

} // namespace

bool hardwareDecodes(rcs_codec codec) {
    if (codec == RCS_CODEC_VP9) {
        static const bool registered = [] {
            if (__builtin_available(macOS 11.0, iOS 26.2, *)) {
                VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_VP9);
                return true;
            }
            return false;
        }();
        if (!registered) {
            return false;
        }
    }
    return VTIsHardwareDecodeSupported(codecType(codec));
}

std::vector<EncoderEntry> videoEncoders() {
    std::vector<EncoderEntry> result;
    CFRef<CFArrayRef> list;
    if (VTCopyVideoEncoderList(nullptr, list.out()) != noErr || !list) {
        return result;
    }
    for (NSDictionary* entry in (__bridge NSArray*)list.value) {
        EncoderEntry& item = result.emplace_back();
        item.type = [entry[(__bridge NSString*)kVTVideoEncoderList_CodecType] unsignedIntValue];
        item.hardware = [entry[(__bridge NSString*)kVTVideoEncoderList_IsHardwareAccelerated] boolValue];
        item.id = std::shared_ptr<const void>(CFBridgingRetain(entry[(__bridge NSString*)kVTVideoEncoderList_EncoderID]),
                                              [](const void* value) {
                                                  if (value) {
                                                      CFRelease(value);
                                                  }
                                              });
        item.name = text(entry[(__bridge NSString*)kVTVideoEncoderList_DisplayName]);
    }
    return result;
}

std::vector<CodecImplementation> encoderImplementations() {
    std::vector<CodecImplementation> result;
    const auto encoders = videoEncoders();
    for (const rcs_codec codec : encodedCodecs) {
        const auto limit = codecLimit(codec);
        for (const bool hardware : {true, false}) {
            for (const auto& entry : encoders) {
                // VideoToolbox's CPU HEVC encoder needs ~87 ms per 1080p frame, too
                // slow for real time, as on Windows; its CPU H.264 encoder needs ~8 ms.
                if (entry.type == codecType(codec) && entry.hardware == hardware &&
                    (hardware || codec == RCS_CODEC_H264)) {
                    const auto features = hardware ? realTime(entry.type) : RealTime{};
                    result.push_back({codec, RCS_CODEC_ENCODER,
                                      hardware ? RCS_ACCELERATION_HARDWARE : RCS_ACCELERATION_SOFTWARE,
                                      RCS_BACKEND_VIDEOTOOLBOX, 0, limit.width, limit.height, encoderFormats(entry),
                                      entry.name, features.longTermReferences, features.layers ? 2u : 1u});
                }
            }
        }
    }
    return result;
}

std::vector<CodecImplementation> decoderImplementations() {
    std::vector<CodecImplementation> result;
    for (const rcs_codec codec : decodedCodecs) {
        const auto limit = codecLimit(codec);
        if (hardwareDecodes(codec)) {
            result.push_back({codec, RCS_CODEC_DECODER, RCS_ACCELERATION_HARDWARE, RCS_BACKEND_VIDEOTOOLBOX, 0,
                              limit.width, limit.height, decoderFormats(codec, true), decoderName(codec, true)});
        }
        if (softwareDecodes(codec)) {
            result.push_back({codec, RCS_CODEC_DECODER, RCS_ACCELERATION_SOFTWARE, RCS_BACKEND_VIDEOTOOLBOX, 0,
                              limit.width, limit.height, decoderFormats(codec, false), decoderName(codec, false)});
        }
    }
    return result;
}

} // namespace rcsdk::platform::apple
