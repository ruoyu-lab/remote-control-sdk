// One list of the system's codecs and the libraries', and which of them
// encodes or decodes a stream.
#include "codecs/codecs.hpp"
#include "codecs/libraries.hpp"

#include <algorithm>
#include <thread>

namespace rcsdk::codecs {
namespace {

using platform::CodecImplementation;

struct Library {
    rcs_backend backend;
    const char* name;   // "libvpx"
    const char* option; // "RCSDK_WITH_LIBVPX"
    // Null where the build left the library out, or for a direction it has not.
    std::vector<CodecImplementation> (*implementations)();
    std::unique_ptr<platform::VideoEncoder> (*encoder)();
    std::unique_ptr<platform::VideoDecoder> (*decoder)();
};

constexpr Library libraries[] = {
#if RCSDK_WITH_OPENH264
    {RCS_BACKEND_OPENH264, "OpenH264", "RCSDK_WITH_OPENH264", openh264::implementations, openh264::encoder,
     openh264::decoder},
#else
    {RCS_BACKEND_OPENH264, "OpenH264", "RCSDK_WITH_OPENH264", nullptr, nullptr, nullptr},
#endif
#if RCSDK_WITH_LIBVPX
    {RCS_BACKEND_LIBVPX, "libvpx", "RCSDK_WITH_LIBVPX", libvpx::implementations, libvpx::encoder, libvpx::decoder},
#else
    {RCS_BACKEND_LIBVPX, "libvpx", "RCSDK_WITH_LIBVPX", nullptr, nullptr, nullptr},
#endif
#if RCSDK_WITH_LIBAOM
    {RCS_BACKEND_LIBAOM, "libaom", "RCSDK_WITH_LIBAOM", libaom::implementations, libaom::encoder, nullptr},
#else
    {RCS_BACKEND_LIBAOM, "libaom", "RCSDK_WITH_LIBAOM", nullptr, nullptr, nullptr},
#endif
#if RCSDK_WITH_DAV1D
    {RCS_BACKEND_DAV1D, "dav1d", "RCSDK_WITH_DAV1D", dav1d::implementations, nullptr, dav1d::decoder},
#else
    {RCS_BACKEND_DAV1D, "dav1d", "RCSDK_WITH_DAV1D", nullptr, nullptr, nullptr},
#endif
};

const Library* libraryOf(rcs_backend backend) noexcept {
    const auto found = std::find_if(std::begin(libraries), std::end(libraries),
                                    [&](const Library& l) { return l.backend == backend; });
    return found == std::end(libraries) ? nullptr : found;
}

// The first implementation of `codec`, `direction` and `acceleration` (HARDWARE
// or SOFTWARE) with `backend` (AUTO: any).
const CodecImplementation* first(std::span<const CodecImplementation> implementations, rcs_codec codec,
                                 rcs_codec_direction direction, rcs_acceleration acceleration, rcs_backend backend) {
    const auto found = std::find_if(implementations.begin(), implementations.end(), [&](const CodecImplementation& i) {
        return i.codec == codec && i.direction == direction && i.acceleration == acceleration &&
               (backend == RCS_BACKEND_AUTO || i.backend == backend);
    });
    return found == implementations.end() ? nullptr : &*found;
}

} // namespace

std::vector<CodecImplementation> implementations() {
    const auto system = platform::codecImplementations();
    std::vector<CodecImplementation> library;
    for (const auto& l : libraries) {
        if (l.implementations) {
            const auto list = l.implementations();
            library.insert(library.end(), list.begin(), list.end());
        }
    }
    // Grouped by codec and direction: hardware, then software, each in the
    // order its source lists it.
    std::vector<CodecImplementation> all;
    const auto take = [&](const std::vector<CodecImplementation>& from, rcs_codec codec,
                          rcs_codec_direction direction, rcs_acceleration acceleration) {
        std::copy_if(from.begin(), from.end(), std::back_inserter(all), [&](const CodecImplementation& i) {
            return i.codec == codec && i.direction == direction && i.acceleration == acceleration;
        });
    };
    for (const rcs_codec codec : standardCodecs) {
        for (const rcs_codec_direction direction : {RCS_CODEC_ENCODER, RCS_CODEC_DECODER}) {
            take(system, codec, direction, RCS_ACCELERATION_HARDWARE);
            take(library, codec, direction, RCS_ACCELERATION_HARDWARE);
            const bool systemFirst = codec == RCS_CODEC_H264;
            take(systemFirst ? system : library, codec, direction, RCS_ACCELERATION_SOFTWARE);
            take(systemFirst ? library : system, codec, direction, RCS_ACCELERATION_SOFTWARE);
        }
    }
    return all;
}

std::string missing(rcs_backend backend) {
    const Library* l = libraryOf(backend);
    if (!l || l->implementations) {
        return {};
    }
    return std::string("this build has no ") + l->name + " (" + l->option + "=OFF)";
}

unsigned threads(uint32_t wanted) noexcept {
    if (wanted) {
        return wanted;
    }
    return std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
}

rcs_acceleration streamAcceleration(rcs_acceleration acceleration, rcs_backend backend, rcs_codec codec,
                                    rcs_codec_direction direction, rcs_picture_format format, uint32_t width,
                                    uint32_t height, uint64_t adapter,
                                    std::span<const CodecImplementation> implementations) {
    if (acceleration != RCS_ACCELERATION_HARDWARE_FIRST) {
        return acceleration;
    }
    // Hardware on the device's GPU (`adapter`): Media Foundation's decoders,
    // probed on the default GPU, decode on whichever GPU the device is on.
    const auto onAdapter = [&](const CodecImplementation& i) {
        return !adapter || !i.adapterLuid || i.adapterLuid == adapter ||
               (direction == RCS_CODEC_DECODER && i.backend == RCS_BACKEND_MEDIA_FOUNDATION);
    };
    const auto takes = [&](rcs_acceleration kind) {
        return std::any_of(implementations.begin(), implementations.end(), [&](const CodecImplementation& i) {
            return i.codec == codec && i.direction == direction && i.acceleration == kind &&
                   (backend == RCS_BACKEND_AUTO || i.backend == backend) &&
                   (kind != RCS_ACCELERATION_HARDWARE || onAdapter(i)) &&
                   (i.pictureFormats & (1u << format)) && width <= i.maxWidth && height <= i.maxHeight;
        });
    };
    // Where neither takes it, the hardware one says why.
    return takes(RCS_ACCELERATION_HARDWARE) || !takes(RCS_ACCELERATION_SOFTWARE) ? RCS_ACCELERATION_HARDWARE
                                                                                : RCS_ACCELERATION_SOFTWARE;
}

std::unique_ptr<platform::VideoEncoder> createEncoder(const platform::EncoderConfig& config,
                                                      std::span<const CodecImplementation> implementations) {
    if (const auto why = missing(config.backend); !why.empty()) {
        fail(RCS_ERROR_UNSUPPORTED, std::string("No ") + backendName(config.backend) + " encoder: " + why);
    }
    const auto* i = first(implementations, config.codec, RCS_CODEC_ENCODER, config.acceleration, config.backend);
    if (i && libraryBackend(i->backend)) {
        const Library* l = libraryOf(i->backend);
        if (!l->encoder) {
            fail(RCS_ERROR_UNSUPPORTED, std::string(l->name) + " does not encode");
        }
        return l->encoder();
    }
    return platform::createVideoEncoder(config);
}

std::unique_ptr<platform::VideoDecoder> createDecoder(const platform::DecoderConfig& config,
                                                      std::span<const CodecImplementation> implementations) {
    if (const auto why = missing(config.backend); !why.empty()) {
        fail(RCS_ERROR_UNSUPPORTED, std::string("No ") + backendName(config.backend) + " decoder: " + why);
    }
    const auto* i = first(implementations, config.codec, RCS_CODEC_DECODER, config.acceleration, config.backend);
    if (i && libraryBackend(i->backend)) {
        const Library* l = libraryOf(i->backend);
        if (!l->decoder) {
            fail(RCS_ERROR_UNSUPPORTED, std::string(l->name) + " does not decode");
        }
        return l->decoder();
    }
    return platform::createVideoDecoder(config);
}

} // namespace rcsdk::codecs
