// CoreFoundation ownership and codec helpers for the Apple backends (macOS and iOS).
#pragma once

#include "media/video.hpp"
#include "util/util.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rcsdk::platform::apple {

// Owns one CoreFoundation reference (released with CFRelease), in the manner
// of win::Com.
template <class T>
class CFRef {
public:
    T value = nullptr;
    CFRef() = default;
    explicit CFRef(T owned) : value(owned) {}
    ~CFRef() { reset(); }
    CFRef(const CFRef&) = delete;
    CFRef& operator=(const CFRef&) = delete;
    CFRef(CFRef&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    CFRef& operator=(CFRef&& other) noexcept {
        reset(std::exchange(other.value, nullptr));
        return *this;
    }
    T* out() noexcept {
        reset();
        return &value;
    }
    void reset(T owned = nullptr) noexcept {
        if (value) {
            CFRelease(value);
        }
        value = owned;
    }
    explicit operator bool() const noexcept { return value != nullptr; }
};

inline CFRef<CFMutableDictionaryRef> dictionary() {
    return CFRef<CFMutableDictionaryRef>(
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
}

inline void setNumber(CFMutableDictionaryRef dictionary, CFStringRef key, int64_t value) {
    CFRef<CFNumberRef> number(CFNumberCreate(nullptr, kCFNumberSInt64Type, &value));
    CFDictionarySetValue(dictionary, key, number.value);
}

// Shares ownership of a pixel buffer through VideoFrame::surface.
inline std::shared_ptr<void> retainSurface(CVPixelBufferRef buffer) {
    CVPixelBufferRetain(buffer);
    return {buffer, [](void* value) { CVPixelBufferRelease(static_cast<CVPixelBufferRef>(value)); }};
}

// IOSurface-backed, Metal-compatible pixel buffer attributes (BGRA or NV12).
inline CFRef<CFMutableDictionaryRef> surfaceAttributes(uint32_t width, uint32_t height,
                                                       OSType format = kCVPixelFormatType_32BGRA) {
    auto attributes = dictionary();
    setNumber(attributes.value, kCVPixelBufferPixelFormatTypeKey, format);
    if (width && height) {
        setNumber(attributes.value, kCVPixelBufferWidthKey, width);
        setNumber(attributes.value, kCVPixelBufferHeightKey, height);
    }
    auto surface = dictionary();
    CFDictionarySetValue(attributes.value, kCVPixelBufferIOSurfacePropertiesKey, surface.value);
    CFDictionarySetValue(attributes.value, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);
    return attributes;
}

// The pixel buffer format of a YCbCr picture: bi-planar, 4:2:0 or 4:4:4, 8 or
// 10 bits, video (limited) or full range (420v, 420f, x420, xf20, 444v, 444f,
// x444, xf44).
inline OSType pixelFormatOf(const Picture& picture) {
    const bool full = picture.range == RCS_RANGE_FULL;
    const bool wide = bitDepth(picture.format) > 8;
    if (chroma444(picture.format)) {
        return wide ? (full ? kCVPixelFormatType_444YpCbCr10BiPlanarFullRange
                            : kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange)
                    : (full ? kCVPixelFormatType_444YpCbCr8BiPlanarFullRange
                            : kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange);
    }
    return wide ? (full ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange
                        : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange)
                : (full ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                        : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
}

// The public layout of a pixel buffer format; 0 for one the SDK does not hand out.
inline rcs_pixel_format layoutOf(OSType format) noexcept {
    switch (format) {
    case kCVPixelFormatType_32BGRA:
        return RCS_PIXEL_BGRA8;
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
        return RCS_PIXEL_NV12;
    case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
    case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
        return RCS_PIXEL_P010;
    case kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange:
    case kCVPixelFormatType_444YpCbCr8BiPlanarFullRange:
        return RCS_PIXEL_NV24;
    case kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange:
    case kCVPixelFormatType_444YpCbCr10BiPlanarFullRange:
        return RCS_PIXEL_P410;
    default:
        return 0;
    }
}

// The VideoToolbox colour description of a picture (kCVImageBuffer* values).
struct ColorKeys {
    CFStringRef primaries, transfer, matrix;
};
inline ColorKeys colorKeysOf(const Picture& picture) noexcept {
    const bool high = hdr(picture.format);
    return {high ? kCVImageBufferColorPrimaries_ITU_R_2020 : kCVImageBufferColorPrimaries_ITU_R_709_2,
            high ? kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ : kCVImageBufferTransferFunction_ITU_R_709_2,
            picture.matrix == RCS_MATRIX_BT2020  ? kCVImageBufferYCbCrMatrix_ITU_R_2020
            : picture.matrix == RCS_MATRIX_BT601 ? kCVImageBufferYCbCrMatrix_ITU_R_601_4
                                                 : kCVImageBufferYCbCrMatrix_ITU_R_709_2};
}

// One entry of VideoToolbox's encoder list (codecs.mm).
struct EncoderEntry {
    CMVideoCodecType type = 0;
    bool hardware = false;
    std::shared_ptr<const void> id; // CFStringRef, for kVTVideoEncoderSpecification_EncoderID.
    std::string name;               // e.g. "Apple HEVC (HW)".
};
std::vector<EncoderEntry> videoEncoders();

// VideoToolbox lists no decoders; they are named after its encoders.
inline std::string decoderName(rcs_codec codec, bool hardware) {
    return std::string("Apple ") + codecName(codec) + (hardware ? " decoder (HW)" : " decoder (SW)");
}

// CoreMedia codec type of a codec this backend implements.
inline CMVideoCodecType codecType(rcs_codec codec) {
    switch (codec) {
    case RCS_CODEC_H264:
        return kCMVideoCodecType_H264;
    case RCS_CODEC_HEVC:
        return kCMVideoCodecType_HEVC;
    case RCS_CODEC_VP9:
        return kCMVideoCodecType_VP9;
    case RCS_CODEC_AV1:
        return kCMVideoCodecType_AV1;
    default:
        fail(RCS_ERROR_UNSUPPORTED, std::string("VideoToolbox does not implement ") + codecName(codec));
    }
}

// As win::check: fails with `category` unless `status` is noErr.
inline void check(OSStatus status, const char* operation, rcs_result category = RCS_ERROR_INTERNAL) {
    if (status != noErr) {
        fail(category, std::string(operation) + " failed (OSStatus " + std::to_string(status) + ")");
    }
}

} // namespace rcsdk::platform::apple
