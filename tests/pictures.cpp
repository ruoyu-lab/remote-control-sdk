// Picture sizes: what a stream may be, what each codec takes, fitting a size
// into a limit; and where a turned display's panel image holds the desktop.
#include "media/picture_size.hpp"
#include "support/check.hpp"
#include "support/panel_image.hpp"

#include <tuple>

using namespace rcsdk;

namespace {

void sizes() {
    // Any stream: even, at least 16x16, at most 8192 a side and 35 651 584 pixels.
    CHECK(validPictureSize(16, 16) && validPictureSize(8192, 4352) && validPictureSize(4352, 8192));
    CHECK(validPictureSize(5970, 5970)); // 35 640 900 pixels.
    CHECK(!validPictureSize(14, 16) && !validPictureSize(17, 16) && !validPictureSize(8194, 16));
    CHECK(!validPictureSize(8192, 4354) && !validPictureSize(6000, 6000) && !validPictureSize(8192, 8192));

    // H.264: 4096 a side and 36 864 macroblocks, either way round.
    const auto h264 = codecLimit(RCS_CODEC_H264);
    CHECK(takes(h264, 4096, 2304) && takes(h264, 2304, 4096) && takes(h264, 4096, 2160) && takes(h264, 2160, 3840));
    CHECK(!takes(h264, 4096, 2320) && !takes(h264, 5120, 2880) && !takes(h264, 4098, 16) && !takes(h264, 3840, 3840));
    CHECK(takes(h264, 4090, 2300)); // Rounded up: 256 x 144 macroblocks.
    // HEVC, AV1 and the hosts' own: all a stream may be.
    for (const rcs_codec codec : {RCS_CODEC_HEVC, RCS_CODEC_AV1, RCS_CODEC_HOST_FIRST}) {
        const auto limit = codecLimit(codec);
        CHECK(takes(limit, 8192, 4352) && takes(limit, 4352, 8192) && takes(limit, 7680, 4320));
        CHECK(!takes(limit, 8192, 4354) && !takes(limit, 8194, 2000));
    }
    CHECK(takes(narrowed(maxPicture, 4096, 2304), 4096, 2304) && !takes(narrowed(maxPicture, 4096, 2304), 2304, 4096));

    // Fitting: as it is when it fits; else the largest of its aspect ratio, even.
    CHECK((fitPicture(1920, 1080, h264) == PictureSize{1920, 1080}));
    CHECK((fitPicture(5120, 2880, h264) == PictureSize{4096, 2304}));
    CHECK((fitPicture(7680, 4320, h264) == PictureSize{4096, 2304}));
    CHECK((fitPicture(2880, 5120, h264) == PictureSize{2304, 4096}));
    CHECK((fitPicture(3000, 1000, narrowed(maxPicture, 2560, 1440)) == PictureSize{2560, 852}));
    CHECK((fitPicture(1920, 1080, narrowed(maxPicture, 16, 16)) == PictureSize{16, 16})); // Never below 16x16.
    for (const auto& [width, height, limit] :
         {std::tuple{3840u, 3840u, h264}, std::tuple{8192u, 5120u, maxPicture}, std::tuple{8192u, 8192u, maxPicture},
          std::tuple{7680u, 4800u, codecLimit(RCS_CODEC_HEVC)}, std::tuple{4000u, 3000u, h264}}) {
        const auto fitted = fitPicture(width, height, limit);
        CHECK(takes(limit, fitted.width, fitted.height) && fitted.width % 2 == 0 && fitted.height % 2 == 0);
        // The aspect ratio within rounding, and nearly all the room there is.
        CHECK(std::abs(double(fitted.width) / fitted.height - double(width) / height) < 0.01);
        const double used = double(fitted.width) * fitted.height / (double(limit.area) * limit.block * limit.block);
        CHECK(used > 0.98 || fitted.width >= limit.width - 2 || fitted.height >= limit.height - 2);
    }

    // What reasons say.
    CHECK(limitText(h264) == "4096 a side and 36 864 macroblocks (e.g. 4096x2304)");
    CHECK(limitText(maxPicture) == "8192 a side and 35 651 584 pixels (e.g. 8192x4352)");
    CHECK(limitText(narrowed(h264, 4096, 2304)) == "4096 wide and 2304 high");
    CHECK(limitText(narrowed(maxPicture, 5120, 2880)) == "5120 wide and 2880 high");
}

// The panel mapping against Microsoft's documentation and its sample.
void panels() {
    using test::panelImage;
    using test::panelPoint;
    using test::panelSize;
    // "if the desktop is set to 768x1024 at 90 degrees rotation, AcquireNextFrame
    // returns a 1024x768 surface with the desktop image rotated within it".
    CHECK((panelSize(1, 768, 1024) == std::pair{1024u, 768u}) && (panelSize(2, 1024, 768) == std::pair{1024u, 768u}) &&
          (panelSize(3, 768, 1024) == std::pair{1024u, 768u}));
    // The sample (DisplayManager.cpp, SetDirtyVert) maps a dirty rectangle D of
    // the texture to the desktop and pairs the corners: with W x H the
    // desktop's size, each desktop corner (Pos) and the texel it shows
    // (TexCoord), for D = {left 10, top 20, right 30, bottom 50}.
    constexpr double left = 10, top = 20, right = 30, bottom = 50;
    constexpr uint32_t width = 768, height = 1024;
    using Pair = std::tuple<double, double, double, double>; // Desktop x, y; texel x, y.
    // ROTATE90: DestDirty {W - bottom, left, W - top, right}.
    for (const auto& [x, y, tx, ty] :
         {Pair{width - bottom, right, right, bottom}, Pair{width - bottom, left, left, bottom},
          Pair{width - top, right, right, top}, Pair{width - top, left, left, top}}) {
        CHECK((panelPoint(1, x, y, width, height) == std::pair{tx, ty}));
    }
    // ROTATE180: DestDirty {W - right, H - bottom, W - left, H - top}.
    for (const auto& [x, y, tx, ty] :
         {Pair{width - right, height - top, right, top}, Pair{width - right, height - bottom, right, bottom},
          Pair{width - left, height - top, left, top}, Pair{width - left, height - bottom, left, bottom}}) {
        CHECK((panelPoint(2, x, y, width, height) == std::pair{tx, ty}));
    }
    // ROTATE270: DestDirty {top, H - right, bottom, H - left}.
    for (const auto& [x, y, tx, ty] :
         {Pair{top, height - left, left, top}, Pair{top, height - right, right, top},
          Pair{bottom, height - left, left, bottom}, Pair{bottom, height - right, right, bottom}}) {
        CHECK((panelPoint(3, x, y, width, height) == std::pair{tx, ty}));
    }
    // A 3x2 desktop, A B C over D E F, in the panel of each turn: a quarter
    // clockwise holds it a quarter the other way (its top row up the left side).
    const std::vector<uint32_t> desktop{'A', 'B', 'C', 'D', 'E', 'F'};
    CHECK((panelImage(0, desktop, 3, 2) == desktop));
    CHECK((panelImage(1, desktop, 3, 2) == std::vector<uint32_t>{'C', 'F', 'B', 'E', 'A', 'D'}));
    CHECK((panelImage(2, desktop, 3, 2) == std::vector<uint32_t>{'F', 'E', 'D', 'C', 'B', 'A'}));
    CHECK((panelImage(3, desktop, 3, 2) == std::vector<uint32_t>{'D', 'A', 'E', 'B', 'F', 'C'}));
}

} // namespace

int main() {
    return runTest("pictures", [] {
        sizes();
        panels();
    });
}
