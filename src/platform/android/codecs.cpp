// The NDK has no list of MediaCodec's codecs before Android 15 (and none
// without Java before that): the hardware codec of a type is the one MediaCodec
// picks for it when that one is no software codec, and the software codecs are
// Android's own, which every device has under their known names. How large a
// picture each takes, its configuration says: it is configured for the
// largest sizes either way round until it takes one. Which picture formats a
// decoder takes, decoding a keyframe of each says, as on Apple's systems.
#include "platform/android/codecs.hpp"

#include "media/picture_size.hpp"
#include "platform/decode_probes.hpp"

#include <dlfcn.h>

#include <array>
#include <mutex>

namespace rcsdk::platform::android {
namespace {

// Android's software codecs: Codec2's, then OpenMAX's of older systems.
struct SoftwareNames {
    rcs_codec codec;
    rcs_codec_direction direction;
    std::array<const char*, 2> names;
};
constexpr SoftwareNames softwareCodecs[] = {
    {RCS_CODEC_H264, RCS_CODEC_ENCODER, {"c2.android.avc.encoder", "OMX.google.h264.encoder"}},
    {RCS_CODEC_H264, RCS_CODEC_DECODER, {"c2.android.avc.decoder", "OMX.google.h264.decoder"}},
    {RCS_CODEC_HEVC, RCS_CODEC_DECODER, {"c2.android.hevc.decoder", "OMX.google.hevc.decoder"}},
    {RCS_CODEC_VP8, RCS_CODEC_DECODER, {"c2.android.vp8.decoder", "OMX.google.vp8.decoder"}},
    {RCS_CODEC_VP9, RCS_CODEC_DECODER, {"c2.android.vp9.decoder", "OMX.google.vp9.decoder"}},
    {RCS_CODEC_AV1, RCS_CODEC_DECODER, {"c2.android.av1.decoder", "c2.android.av1-dav1d.decoder"}},
};

bool software(const std::string& name) noexcept {
    return name.starts_with("c2.android.") || name.starts_with("OMX.google.");
}

std::string nameOf(AMediaCodec* codec) {
    char* name = nullptr;
    std::string result;
    if (AMediaCodec_getName(codec, &name) == AMEDIA_OK && name) {
        result = name;
        AMediaCodec_releaseName(codec, name);
    }
    return result;
}

// What MediaCodec picks for the type, when that runs on the hardware. The
// emulator's c2.goldfish. decoders hand pictures to its host computer's,
// which return full-range ones in limited range: no hardware of a device.
std::string hardwareCodec(const char* mime, rcs_codec_direction direction) {
    const Codec codec(direction == RCS_CODEC_ENCODER ? AMediaCodec_createEncoderByType(mime)
                                                     : AMediaCodec_createDecoderByType(mime));
    if (!codec) {
        return {};
    }
    std::string name = nameOf(codec.get());
    return software(name) || name.starts_with("c2.goldfish.") ? std::string() : name;
}

std::string softwareCodec(const SoftwareNames& entry) {
    for (const char* name : entry.names) {
        if (Codec(AMediaCodec_createCodecByName(name))) {
            return name;
        }
    }
    return {};
}

// Whether the codec `name` takes pictures of width x height.
bool configures(const std::string& name, rcs_codec codec, rcs_codec_direction direction, uint32_t width,
                uint32_t height) {
    const Codec instance(AMediaCodec_createCodecByName(name.c_str()));
    if (!instance) {
        return false;
    }
    const Format format(AMediaFormat_new());
    AMediaFormat_setString(format.get(), AMEDIAFORMAT_KEY_MIME, mimeOf(codec));
    AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_WIDTH, int32_t(width));
    AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_HEIGHT, int32_t(height));
    const bool encoder = direction == RCS_CODEC_ENCODER;
    if (encoder) {
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x7F000789); // Surface input.
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_BIT_RATE, 10'000'000);
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_FRAME_RATE, 30);
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
    }
    return AMediaCodec_configure(instance.get(), format.get(), nullptr, nullptr,
                                 encoder ? AMEDIACODEC_CONFIGURE_FLAG_ENCODE : 0) == AMEDIA_OK;
}

// The longest side of the largest 16:9 picture it takes either way round
// (portrait screens too); 0 for none of them.
uint32_t longestSide(const std::string& name, rcs_codec codec, rcs_codec_direction direction) {
    static constexpr PictureSize sizes[] = {{8192, 4352}, {4096, 2304}, {3840, 2160}, {2560, 1440},
                                            {2048, 1152}, {1920, 1080}, {1280, 720}};
    const auto limit = codecLimit(codec);
    for (const auto size : sizes) {
        if (takes(limit, size.width, size.height) && configures(name, codec, direction, size.width, size.height) &&
            configures(name, codec, direction, size.height, size.width)) {
            return size.width;
        }
    }
    return 0;
}

// The picture formats the decoder `name` takes: 4:2:0 8-bit, and those of
// the probes it decodes.
uint32_t decoderFormats(const std::string& name, rcs_codec codec) {
    uint32_t formats = defaultPictureFormats;
    for (const auto& probe : probes::all) {
        if (probe.codec == codec && decodes(name, codec, probe.format, probe.config, probe.data)) {
            formats |= 1u << probe.format;
        }
    }
    return formats;
}

std::vector<CodecImplementation> list() {
    serveBinderCalls();
    std::vector<CodecImplementation> result;
    for (const rcs_codec_direction direction : {RCS_CODEC_ENCODER, RCS_CODEC_DECODER}) {
        for (const rcs_codec codec : standardCodecs) {
            const auto add = [&](rcs_acceleration acceleration, std::string name) {
                const uint32_t side = name.empty() ? 0 : longestSide(name, codec, direction);
                if (side) {
                    const uint32_t formats =
                        direction == RCS_CODEC_DECODER ? decoderFormats(name, codec) : defaultPictureFormats;
                    result.push_back({codec, direction, acceleration, RCS_BACKEND_MEDIACODEC, 0, side, side,
                                      formats, std::move(name)});
                }
            };
            add(RCS_ACCELERATION_HARDWARE, hardwareCodec(mimeOf(codec), direction));
            for (const auto& entry : softwareCodecs) {
                if (entry.codec == codec && entry.direction == direction) {
                    add(RCS_ACCELERATION_SOFTWARE, softwareCodec(entry));
                }
            }
        }
    }
    return result;
}

} // namespace

// libbinder_ndk's ABinderProcess_startThreadPool, which the NDK's headers
// leave out (apps need not call it; in an app it does nothing).
void serveBinderCalls() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (void* library = dlopen("libbinder_ndk.so", RTLD_NOW)) {
            if (auto* start = reinterpret_cast<void (*)()>(dlsym(library, "ABinderProcess_startThreadPool"))) {
                start();
            }
        }
    });
}

const char* mimeOf(rcs_codec codec) noexcept {
    switch (codec) {
    case RCS_CODEC_H264:
        return "video/avc";
    case RCS_CODEC_HEVC:
        return "video/hevc";
    case RCS_CODEC_VP8:
        return "video/x-vnd.on2.vp8";
    case RCS_CODEC_VP9:
        return "video/x-vnd.on2.vp9";
    case RCS_CODEC_AV1:
        return "video/av01";
    default:
        return nullptr;
    }
}

std::string statusText(media_status_t status) {
    const char* name = "error";
    switch (status) {
    case AMEDIA_ERROR_MALFORMED:
        name = "AMEDIA_ERROR_MALFORMED";
        break;
    case AMEDIA_ERROR_UNSUPPORTED:
        name = "AMEDIA_ERROR_UNSUPPORTED";
        break;
    case AMEDIA_ERROR_INVALID_OBJECT:
        name = "AMEDIA_ERROR_INVALID_OBJECT";
        break;
    case AMEDIA_ERROR_INVALID_PARAMETER:
        name = "AMEDIA_ERROR_INVALID_PARAMETER";
        break;
    case AMEDIA_ERROR_INVALID_OPERATION:
        name = "AMEDIA_ERROR_INVALID_OPERATION";
        break;
    case AMEDIA_ERROR_IO:
        name = "AMEDIA_ERROR_IO";
        break;
    case AMEDIA_ERROR_WOULD_BLOCK:
        name = "AMEDIA_ERROR_WOULD_BLOCK";
        break;
    case AMEDIACODEC_ERROR_INSUFFICIENT_RESOURCE:
        name = "AMEDIACODEC_ERROR_INSUFFICIENT_RESOURCE";
        break;
    case AMEDIACODEC_ERROR_RECLAIMED:
        name = "AMEDIACODEC_ERROR_RECLAIMED";
        break;
    case AMEDIA_ERROR_UNKNOWN:
        name = "AMEDIA_ERROR_UNKNOWN";
        break;
    default:
        break;
    }
    return std::string(name) + " (" + std::to_string(int(status)) + ")";
}

void check(media_status_t status, const char* what, rcs_result result) {
    if (status != AMEDIA_OK) {
        fail(result, std::string(what) + " failed: " + statusText(status));
    }
}

void setColor(AMediaFormat* format, const Picture& picture) {
    const bool high = hdr(picture.format);
    // MediaFormat.COLOR_STANDARD_BT2020 (6), _BT601_NTSC (4), _BT709 (1).
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_STANDARD,
                          high ? 6 : picture.matrix == RCS_MATRIX_BT601 ? 4 : 1);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_RANGE, picture.range == RCS_RANGE_FULL ? 1 : 2);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_TRANSFER, high ? 6 /* ST2084 */ : 3 /* SDR video */);
}

std::vector<CodecImplementation> implementations() {
    static const auto value = list(); // Opening every codec takes a while.
    return value;
}

const CodecImplementation& implementation(rcs_codec codec, rcs_codec_direction direction, rcs_acceleration acceleration,
                                          rcs_backend backend) {
    static const auto all = implementations();
    if (backend != RCS_BACKEND_AUTO && backend != RCS_BACKEND_MEDIACODEC) {
        fail(RCS_ERROR_UNSUPPORTED, "Android's codecs are MediaCodec's (RCS_BACKEND_MEDIACODEC)");
    }
    for (const auto& entry : all) {
        if (entry.codec == codec && entry.direction == direction && entry.acceleration == acceleration) {
            return entry;
        }
    }
    fail(RCS_ERROR_UNSUPPORTED, std::string("This device has no ") +
                                    (acceleration == RCS_ACCELERATION_SOFTWARE ? "software " : "hardware ") +
                                    codecName(codec) + (direction == RCS_CODEC_ENCODER ? " encoder" : " decoder"));
}

Codec open(const std::string& name) {
    serveBinderCalls();
    Codec codec(AMediaCodec_createCodecByName(name.c_str()));
    if (!codec) {
        fail(RCS_ERROR_CODEC, "MediaCodec could not open " + name +
                                  " (another application may hold every "
                                  "instance it has)");
    }
    return codec;
}

} // namespace rcsdk::platform::android
