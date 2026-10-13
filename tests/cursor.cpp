// Pointers larger than maxCursorSize are scaled down to fit on both
// platforms: shape and hotspot keep their places, colour and alpha average.
#include "media/cursor.hpp"
#include "support/check.hpp"

using namespace rcsdk;

int main() {
    return runTest("cursor", [] {
        CHECK(fittedCursorSize(32, 32).width == 32 && fittedCursorSize(256, 100).height == 100); // Small: as it is.
        CHECK(fittedCursorSize(512, 512).width == 256 && fittedCursorSize(512, 512).height == 256);
        CHECK(fittedCursorSize(600, 300).width == 256 && fittedCursorSize(600, 300).height == 128);
        CHECK(fittedCursorSize(2000, 1).width == 256 && fittedCursorSize(2000, 1).height == 1);

        // 512x384: opaque red on the left half, transparent on the right;
        // the hotspot at the centre.
        CursorImage image;
        image.width = 512;
        image.height = 384;
        image.hotspotX = 256;
        image.hotspotY = 192;
        image.pixels.assign(size_t(image.width) * image.height * 4, 0);
        for (uint32_t y = 0; y < image.height; ++y) {
            for (uint32_t x = 0; x < image.width / 2; ++x) {
                uint8_t* p = image.pixels.data() + (size_t(y) * image.width + x) * 4;
                p[2] = p[3] = 255;
            }
        }
        fitCursor(image);
        CHECK(image.width == 256 && image.height == 192 && image.pixels.size() == size_t(256) * 192 * 4);
        CHECK(image.hotspotX == 128 && image.hotspotY == 96);
        const uint8_t* left = image.pixels.data() + (size_t(10) * 256 + 10) * 4;
        const uint8_t* right = image.pixels.data() + (size_t(10) * 256 + 200) * 4;
        CHECK(left[0] == 0 && left[1] == 0 && left[2] == 255 && left[3] == 255);
        CHECK(right[3] == 0);

        // Opaque blue every other column: alpha averages to about half, and
        // the colour of what is visible stays blue (no dark fringe).
        CursorImage stripes;
        stripes.width = 600;
        stripes.height = 2;
        stripes.pixels.assign(size_t(600) * 2 * 4, 0);
        for (uint32_t x = 0; x < 600; x += 2) {
            for (uint32_t y = 0; y < 2; ++y) {
                uint8_t* p = stripes.pixels.data() + (size_t(y) * 600 + x) * 4;
                p[0] = p[3] = 255;
            }
        }
        fitCursor(stripes);
        CHECK(stripes.width == 256 && stripes.height == 1 && stripes.hotspotX == 0);
        uint64_t alpha = 0;
        for (size_t i = 0; i < stripes.pixels.size(); i += 4) {
            CHECK(stripes.pixels[i] == 255 && stripes.pixels[i + 1] == 0 && stripes.pixels[i + 3] > 0);
            alpha += stripes.pixels[i + 3];
        }
        CHECK(alpha / 256 > 110 && alpha / 256 < 145);
    });
}
