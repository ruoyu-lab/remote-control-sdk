// AV1 decoding in software: dav1d (third_party/dav1d), one picture out for
// each picture in (a frame delay of 1: its threads work on the tiles and
// filters of that one picture). The stream's sequence header (the packets'
// config) goes ahead of every keyframe and of the first packet after a
// reset or a change of it. What dav1d says when a picture fails is the
// failure's reason.
#include "codecs/software_codec.hpp"

#include "rcsdk_dav1d_names.h"
#include <dav1d/dav1d.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace rcsdk::codecs::dav1d {
namespace {

using platform::CodecImplementation;

constexpr uint32_t formats = (1u << RCS_PICTURE_420_8) | (1u << RCS_PICTURE_420_10) | (1u << RCS_PICTURE_444_8) |
                             (1u << RCS_PICTURE_444_10) | (1u << RCS_PICTURE_HDR10_420) |
                             (1u << RCS_PICTURE_HDR10_444);

std::string implementationName() {
    return std::string("dav1d AV1 ") + dav1d_version();
}

class Decoder final : public SoftwareDecoder {
public:
    ~Decoder() override {
        if (context_) {
            dav1d_close(&context_);
        }
    }

    void reset() override {
        dav1d_flush(context_);
        sentConfig_.clear();
    }
    std::string name() const override { return implementationName(); }
    rcs_backend backend() const noexcept override { return RCS_BACKEND_DAV1D; }

private:
    void start(const platform::DecoderConfig& c) override {
        if (c.codec != RCS_CODEC_AV1) {
            fail(RCS_ERROR_UNSUPPORTED, std::string("dav1d does not decode ") + codecName(c.codec));
        }
        if (!(formats & (1u << c.picture.format))) {
            fail(RCS_ERROR_UNSUPPORTED,
                 std::string("dav1d does not decode AV1 in ") + pictureFormatName(c.picture.format));
        }
        Dav1dSettings settings;
        dav1d_default_settings(&settings);
        settings.n_threads = int(codecs::threads(c.softwareThreads));
        settings.max_frame_delay = 1;
        settings.logger.cookie = this;
        settings.logger.callback = [](void* cookie, const char* format, va_list arguments) {
            auto* self = static_cast<Decoder*>(cookie);
            const std::lock_guard lock(self->saidLock_); // Its threads log too.
            std::vsnprintf(self->said_, sizeof self->said_, format, arguments);
        };
        if (const int result = dav1d_open(&context_, &settings); result < 0) {
            failed("Opening dav1d", result);
        }
    }

    [[noreturn]] void failed(const std::string& what, int result) {
        std::string why;
        {
            const std::lock_guard lock(saidLock_);
            why = said_;
        }
        while (!why.empty() && (why.back() == '\n' || why.back() == '.')) {
            why.pop_back();
        }
        fail(RCS_ERROR_CODEC, what + ": " + (why.empty() ? "error " + std::to_string(-result) : why));
    }

    const YuvImage* decodePicture(const EncodedPacket& packet) override {
        {
            const std::lock_guard lock(saidLock_);
            said_[0] = 0;
        }
        const bool header = packet.keyframe || packet.config != sentConfig_;
        struct Data {
            Dav1dData data{};
            ~Data() { dav1d_data_unref(&data); }
        } in;
        const size_t size = (header ? packet.config.size() : 0) + packet.data.size();
        uint8_t* bytes = dav1d_data_create(&in.data, size);
        if (!bytes) {
            fail(RCS_ERROR_CODEC, "dav1d has no memory for a picture's bytes");
        }
        if (header) {
            std::memcpy(bytes, packet.config.data(), packet.config.size());
            sentConfig_ = packet.config;
        }
        std::memcpy(bytes + size - packet.data.size(), packet.data.data(), packet.data.size());
        // With a frame delay of 1, dav1d takes the whole temporal unit at
        // once; it says EAGAIN only while a picture waits to be taken.
        int sent;
        while ((sent = dav1d_send_data(context_, &in.data)) == DAV1D_ERR(EAGAIN)) {
            if (!take(packet.picture)) {
                fail(RCS_ERROR_CODEC, "dav1d neither took an AV1 picture nor gave one");
            }
        }
        if (sent < 0) {
            failed("dav1d could not decode an AV1 picture", sent);
        }
        return take(packet.picture) ? &image_ : nullptr;
    }

    // The next picture dav1d has, into image_: false while it has none.
    bool take(const Picture& stream) {
        Dav1dPicture picture{};
        const int result = dav1d_get_picture(context_, &picture);
        if (result == DAV1D_ERR(EAGAIN)) {
            return false;
        }
        if (result < 0) {
            failed("dav1d could not decode an AV1 picture", result);
        }
        struct Unref {
            Dav1dPicture& picture;
            ~Unref() { dav1d_picture_unref(&picture); }
        } unref{picture};
        const rcs_pixel_format layout = yuvLayoutOf(stream.format);
        const bool full = layout == RCS_PIXEL_NV24 || layout == RCS_PIXEL_P410;
        if (picture.p.bpc != int(bitDepth(stream.format)) ||
            picture.p.layout != (full ? DAV1D_PIXEL_LAYOUT_I444 : DAV1D_PIXEL_LAYOUT_I420)) {
            fail(RCS_ERROR_CODEC, std::string("dav1d decoded another picture format than the stream's (") +
                                      pictureFormatName(stream.format) + ")");
        }
        Planes planes;
        for (int i = 0; i < 3; ++i) {
            planes.plane[i] = static_cast<const uint8_t*>(picture.data[i]);
            planes.stride[i] = size_t(picture.stride[i ? 1 : 0]);
        }
        planes.bits = unsigned(picture.p.bpc);
        fromPlanar(planes, layout, uint32_t(picture.p.w), uint32_t(picture.p.h), picture_);
        image_ = picture_.view();
        return true;
    }

    Dav1dContext* context_ = nullptr;
    std::vector<uint8_t> sentConfig_; // The sequence header dav1d has had since the last reset.
    std::mutex saidLock_;
    char said_[256] = {}; // What dav1d last logged.
    YuvBuffer picture_;
    YuvImage image_;
};

} // namespace

std::vector<CodecImplementation> implementations() {
    return {libraryImplementation(RCS_CODEC_AV1, RCS_CODEC_DECODER, RCS_BACKEND_DAV1D, formats, implementationName(),
                                  false)};
}

std::unique_ptr<platform::VideoDecoder> decoder() {
    return std::make_unique<Decoder>();
}

} // namespace rcsdk::codecs::dav1d
