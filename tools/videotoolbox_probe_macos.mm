// What VideoToolbox on this Mac encodes and decodes, by chroma format and bit
// depth: the profiles each encoder lists, and for each mode the SDK uses (the
// low-latency encoder, the pinned hardware encoder, the software encoder)
// what a 1280x720 encode really produces (its SPS) and whether the hardware
// decoder decodes it back, with which colour. With --streams it prints the
// small keyframes the SDK decodes to learn what the decoders of Apple's
// systems and Android take (src/platform/decode_probes.hpp). Not part of the SDK.
//
//   rcsdk_videotoolbox_probe [--streams]
#include "media/nal.hpp"
#include "media/sampling.hpp"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string fourcc(OSType t) {
    const char s[5] = {char(t >> 24), char(t >> 16), char(t >> 8), char(t), 0};
    return s;
}

std::string text(CFStringRef value) {
    return value ? std::string([(__bridge NSString*)value UTF8String]) : std::string("-");
}

// The decode probes' size: what every decoder takes (Qualcomm's: 96x96 and more).
constexpr int probeWidth = 256, probeHeight = 144;

// A pattern, or (flat) mid-grey in every plane.
CVPixelBufferRef makeBuffer(int width, int height, OSType format, bool flat) {
    NSDictionary* attributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{}};
    CVPixelBufferRef buffer = nullptr;
    if (CVPixelBufferCreate(nullptr, width, height, format, (__bridge CFDictionaryRef)attributes, &buffer) !=
        kCVReturnSuccess) {
        return nullptr;
    }
    CVPixelBufferLockBaseAddress(buffer, 0);
    const size_t planes = CVPixelBufferGetPlaneCount(buffer);
    for (size_t p = 0; p < (planes ? planes : 1); ++p) {
        auto* base = static_cast<uint8_t*>(planes ? CVPixelBufferGetBaseAddressOfPlane(buffer, p)
                                                  : CVPixelBufferGetBaseAddress(buffer));
        const size_t stride =
            planes ? CVPixelBufferGetBytesPerRowOfPlane(buffer, p) : CVPixelBufferGetBytesPerRow(buffer);
        const size_t rows = planes ? CVPixelBufferGetHeightOfPlane(buffer, p) : CVPixelBufferGetHeight(buffer);
        for (size_t y = 0; y < rows; ++y) {
            for (size_t x = 0; x < stride; ++x) {
                base[y * stride + x] = flat ? 0x80 : uint8_t((x * 7 + y * 3 + p * 50) & 0xff);
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(buffer, 0);
    return buffer;
}

void listProfiles(CMVideoCodecType codec, bool hardware, const char* label) {
    NSDictionary* spec = hardware ? @{(id)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder : @YES}
                                  : @{(id)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder : @NO};
    CFStringRef encoder = nullptr;
    CFDictionaryRef supported = nullptr;
    if (VTCopySupportedPropertyDictionaryForEncoder(1280, 720, codec, (__bridge CFDictionaryRef)spec, &encoder,
                                                    &supported) != noErr) {
        std::printf("  %s: none\n", label);
        return;
    }
    NSDictionary* profile = ((__bridge NSDictionary*)supported)[(id)kVTCompressionPropertyKey_ProfileLevel];
    std::printf("  %s (%s):", label, text(encoder).c_str());
    for (NSString* value in profile[(id)kVTPropertySupportedValueListKey]) {
        if (![value hasSuffix:@"AutoLevel"]) {
            continue; // The levels repeat each profile.
        }
        std::printf(" %s", value.UTF8String);
    }
    std::printf("\n");
    CFRelease(supported);
    CFRelease(encoder);
}

struct Encoded {
    std::vector<uint8_t> config, data;
    bool hardware = false;
};

// Two frames of `input` encoded in `mode` (0 low latency, 1 pinned hardware, 2
// software) with `profile` and `pq` (BT.2020 PQ instead of BT.709); the first.
bool encode(CMVideoCodecType codec, int width, int height, OSType input, CFStringRef profile, int mode, bool pq,
            Encoded& out, std::string& why) {
    NSMutableDictionary* spec = [NSMutableDictionary dictionary];
    if (mode == 0) {
        spec[(id)kVTVideoEncoderSpecification_EnableLowLatencyRateControl] = @YES;
    } else if (mode == 1) {
        spec[(id)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder] = @YES;
    } else {
        spec[(id)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder] = @NO;
    }
    NSDictionary* attributes = @{(id)kCVPixelBufferPixelFormatTypeKey : @(input),
                                 (id)kCVPixelBufferWidthKey : @(width), (id)kCVPixelBufferHeightKey : @(height),
                                 (id)kCVPixelBufferIOSurfacePropertiesKey : @{}};
    VTCompressionSessionRef session = nullptr;
    if (OSStatus s = VTCompressionSessionCreate(nullptr, width, height, codec, (__bridge CFDictionaryRef)spec,
                                                (__bridge CFDictionaryRef)attributes, nullptr, nullptr, nullptr,
                                                &session);
        s != noErr) {
        why = "create " + std::to_string(s);
        return false;
    }
    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    if (OSStatus s = VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel, profile); s != noErr) {
        why = "profile " + std::to_string(s);
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        return false;
    }
    VTSessionSetProperty(session, kVTCompressionPropertyKey_ColorPrimaries,
                         pq ? kCVImageBufferColorPrimaries_ITU_R_2020 : kCVImageBufferColorPrimaries_ITU_R_709_2);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_TransferFunction,
                         pq ? kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ
                            : kCVImageBufferTransferFunction_ITU_R_709_2);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_YCbCrMatrix,
                         pq ? kCVImageBufferYCbCrMatrix_ITU_R_2020 : kCVImageBufferYCbCrMatrix_ITU_R_709_2);
    CVPixelBufferRef buffer = makeBuffer(width, height, input, width <= probeWidth);
    __block CMSampleBufferRef first = nullptr;
    for (int i = 0; i < 2; ++i) {
        VTCompressionSessionEncodeFrameWithOutputHandler(session, buffer, CMTimeMake(i, 30), kCMTimeInvalid, nullptr,
                                                         nullptr, ^(OSStatus, VTEncodeInfoFlags, CMSampleBufferRef s) {
                                                           if (s && !first) {
                                                               first = s;
                                                               CFRetain(s);
                                                           }
                                                         });
    }
    VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
    CFBooleanRef hardware = nullptr;
    if (VTSessionCopyProperty(session, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, nullptr,
                              &hardware) == noErr &&
        hardware) {
        out.hardware = CFBooleanGetValue(hardware);
        CFRelease(hardware);
    } else {
        out.hardware = mode != 2;
    }
    CVPixelBufferRelease(buffer);
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    if (!first) {
        why = "no output";
        return false;
    }
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(first);
    size_t count = 0;
    const bool hevc = codec == kCMVideoCodecType_HEVC;
    if (hevc) {
        CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, 0, nullptr, nullptr, &count, nullptr);
    } else {
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, nullptr, nullptr, &count, nullptr);
    }
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* bytes = nullptr;
        size_t size = 0;
        if (hevc) {
            CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, i, &bytes, &size, nullptr, nullptr);
        } else {
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &bytes, &size, nullptr, nullptr);
        }
        rcsdk::nal::appendUnit(out.config, std::span<const uint8_t>(bytes, size));
    }
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(first);
    out.data.resize(CMBlockBufferGetDataLength(block));
    CMBlockBufferCopyDataBytes(block, 0, out.data.size(), out.data.data());
    CFRelease(first);
    return true;
}

// Decodes the keyframe with the hardware (or software) decoder asking for `want`
// (0: its own choice); the output's format and colour, or why not.
std::string decode(CMVideoCodecType codec, const Encoded& encoded, bool hardware, OSType want) {
    std::vector<const uint8_t*> sets;
    std::vector<size_t> sizes;
    for (const auto unit : rcsdk::nal::units(encoded.config)) {
        sets.push_back(unit.data());
        sizes.push_back(unit.size());
    }
    CMVideoFormatDescriptionRef format = nullptr;
    const OSStatus described =
        codec == kCMVideoCodecType_HEVC
            ? CMVideoFormatDescriptionCreateFromHEVCParameterSets(nullptr, sets.size(), sets.data(), sizes.data(), 4,
                                                                  nullptr, &format)
            : CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr, sets.size(), sets.data(), sizes.data(), 4,
                                                                  &format);
    if (described != noErr) {
        return "no format description (" + std::to_string(described) + ")";
    }
    NSDictionary* spec = hardware
                             ? @{(id)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder : @YES,
                                 (id)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder : @YES}
                             : @{(id)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder : @NO};
    NSMutableDictionary* attributes = [@{(id)kCVPixelBufferIOSurfacePropertiesKey : @{}} mutableCopy];
    if (want) {
        attributes[(id)kCVPixelBufferPixelFormatTypeKey] = @(want);
    }
    VTDecompressionSessionRef session = nullptr;
    if (OSStatus s = VTDecompressionSessionCreate(nullptr, format, (__bridge CFDictionaryRef)spec,
                                                  (__bridge CFDictionaryRef)attributes, nullptr, &session);
        s != noErr) {
        CFRelease(format);
        return "no session (" + std::to_string(s) + ")";
    }
    CMBlockBufferRef block = nullptr;
    CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, encoded.data.size(), nullptr, nullptr, 0,
                                       encoded.data.size(), 0, &block);
    CMBlockBufferReplaceDataBytes(encoded.data.data(), block, 0, encoded.data.size());
    const size_t size = encoded.data.size();
    CMSampleBufferRef sample = nullptr;
    CMSampleBufferCreateReady(nullptr, block, format, 1, 0, nullptr, 1, &size, &sample);
    __block std::string result = "no picture";
    OSStatus status = VTDecompressionSessionDecodeFrameWithOutputHandler(
        session, sample, 0, nullptr, ^(OSStatus s, VTDecodeInfoFlags, CVImageBufferRef image, CMTime, CMTime) {
          if (s != noErr || !image) {
              result = "failed (" + std::to_string(s) + ")";
              return;
          }
          result = fourcc(CVPixelBufferGetPixelFormatType(image));
          CFTypeRef matrix = CVBufferCopyAttachment(image, kCVImageBufferYCbCrMatrixKey, nullptr);
          CFTypeRef transfer = CVBufferCopyAttachment(image, kCVImageBufferTransferFunctionKey, nullptr);
          result += " " + text(static_cast<CFStringRef>(matrix)) + " " + text(static_cast<CFStringRef>(transfer));
          if (matrix) {
              CFRelease(matrix);
          }
          if (transfer) {
              CFRelease(transfer);
          }
        });
    VTDecompressionSessionWaitForAsynchronousFrames(session);
    if (status != noErr) {
        result = "failed (" + std::to_string(status) + ")";
    }
    CFRelease(sample);
    CFRelease(block);
    VTDecompressionSessionInvalidate(session);
    CFRelease(session);
    CFRelease(format);
    return result;
}

void printArray(const char* name, const std::vector<uint8_t>& bytes) {
    std::printf("inline constexpr uint8_t %s[] = {", name);
    for (size_t i = 0; i < bytes.size(); ++i) {
        std::printf("%s0x%02x", i ? (i % 16 ? ", " : ",\n    ") : "\n    ", bytes[i]);
    }
    std::printf("};\n");
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        const bool streams = argc > 1 && std::strcmp(argv[1], "--streams") == 0;
        struct Case {
            CMVideoCodecType codec;
            const char* codecName;
            OSType input;
            CFStringRef profile;
            bool pq;
            const char* format;
        };
        const Case cases[] = {
            {kCMVideoCodecType_H264, "H.264", '420v', CFSTR("H264_High_AutoLevel"), false, "4:2:0 8-bit"},
            {kCMVideoCodecType_H264, "H.264", '444v', CFSTR("H264_High444Predictive_AutoLevel"), false, "4:4:4 8-bit"},
            {kCMVideoCodecType_HEVC, "HEVC", '420v', CFSTR("HEVC_Main_AutoLevel"), false, "4:2:0 8-bit"},
            {kCMVideoCodecType_HEVC, "HEVC", 'x420', CFSTR("HEVC_Main10_AutoLevel"), false, "4:2:0 10-bit"},
            {kCMVideoCodecType_HEVC, "HEVC", 'x420', CFSTR("HEVC_Main10_AutoLevel"), true, "HDR10 4:2:0"},
            {kCMVideoCodecType_HEVC, "HEVC", '444v', CFSTR("HEVC_Main444_AutoLevel"), false, "4:4:4 8-bit"},
            {kCMVideoCodecType_HEVC, "HEVC", 'x444', CFSTR("HEVC_Main44410_AutoLevel"), false, "4:4:4 10-bit"},
            {kCMVideoCodecType_HEVC, "HEVC", 'x444', CFSTR("HEVC_Main44410_AutoLevel"), true, "HDR10 4:4:4"},
        };
        if (streams) {
            // Keyframes of the pinned hardware encoder, for the SDK's decode probes.
            const struct {
                const Case* source;
                const char* name;
            } probes[] = {{&cases[1], "h264_444_8"},
                          {&cases[3], "hevc_420_10"},
                          {&cases[5], "hevc_444_8"},
                          {&cases[6], "hevc_444_10"}};
            for (const auto& probe : probes) {
                Encoded encoded;
                std::string why;
                if (!encode(probe.source->codec, probeWidth, probeHeight, probe.source->input, probe.source->profile, 1, false, encoded,
                            why)) {
                    std::fprintf(stderr, "%s: %s\n", probe.name, why.c_str());
                    return 1;
                }
                const auto sampling = rcsdk::keyframeSampling(
                    probe.source->codec == kCMVideoCodecType_HEVC ? RCS_CODEC_HEVC : RCS_CODEC_H264, encoded.config, {});
                std::printf("// %s %s (%s)\n", probe.source->codecName, rcsdk::samplingText(sampling).c_str(),
                            text(probe.source->profile).c_str());
                printArray((std::string(probe.name) + "_config").c_str(), encoded.config);
                printArray((std::string(probe.name) + "_data").c_str(), encoded.data);
            }
            return 0;
        }
        std::printf("Encoder profiles (1280x720):\n");
        listProfiles(kCMVideoCodecType_H264, true, "H.264 hardware");
        listProfiles(kCMVideoCodecType_H264, false, "H.264 software");
        listProfiles(kCMVideoCodecType_HEVC, true, "HEVC hardware");
        listProfiles(kCMVideoCodecType_HEVC, false, "HEVC software");
        const char* modes[] = {"low latency", "hardware", "software"};
        std::printf("\nEncode 1280x720 -> SPS; decode with the hardware / software decoder (its own output format):\n");
        for (const auto& c : cases) {
            for (int mode = 0; mode < 3; ++mode) {
                Encoded encoded;
                std::string why;
                std::printf("  %-5s %-12s %-11s: ", c.codecName, c.format, modes[mode]);
                if (!encode(c.codec, 1280, 720, c.input, c.profile, mode, c.pq, encoded, why)) {
                    std::printf("encode %s\n", why.c_str());
                    continue;
                }
                const auto sampling = rcsdk::keyframeSampling(c.codec == kCMVideoCodecType_HEVC ? RCS_CODEC_HEVC : RCS_CODEC_H264, encoded.config, {});
                std::printf("encodes %s%s; decodes HW %s, SW %s\n", rcsdk::samplingText(sampling).c_str(),
                            encoded.hardware ? "" : " (software)", decode(c.codec, encoded, true, 0).c_str(),
                            decode(c.codec, encoded, false, 0).c_str());
            }
        }
    }
    return 0;
}
