#include <rcsdk/view_geometry.hpp>

#include <cmath>
#include <stdexcept>
int main() {
    const auto check = [](bool x) {
        if (!x) {
            throw std::runtime_error("Incorrect viewport/input coordinates");
        }
    };
    auto fit = rcsdk::viewGeometry(1000, 1000, 1920, 1080);
    uint32_t x = 0, y = 0;
    check(!fit.position(500, 0, 1920, 1080, x, y));
    check(fit.position(fit.x, fit.y, 1920, 1080, x, y) && x == 0 && y == 0);
    check(fit.position(fit.x + fit.width - 1, fit.y + fit.height - 1, 1920, 1080, x, y) && x == 65535 && y == 65535);
    auto crop = rcsdk::viewGeometry(1000, 600, 1920, 1080, rcsdk::ViewScale::nativePixels, 920, 480);
    check(crop.sourceX == 920 && crop.sourceY == 480);
    check(crop.position(999, 599, 1920, 1080, x, y) && x == 65535 && y == 65535);
    check(crop.position(0, 0, 1920, 1080, x, y) && x > 30000 && y > 29000);
    check(crop.position(-100, -100, 1920, 1080, x, y, true) && x > 30000);
    check(rcsdk::viewGeometry(0, 100, 1920, 1080).width == 0);
    auto small = rcsdk::viewGeometry(1000, 600, 320, 180, rcsdk::ViewScale::nativePixels);
    check(small.width == 320 && small.x == 340 && small.height == 180 && small.y == 210);
    rcs_stream_info info = RCS_STREAM_INFO_INIT;
    info.width = 1920;
    info.height = 1080;
    info.content_x = 160;
    info.content_width = 1600;
    info.content_height = 1080;
    x = 0;
    y = 32768;
    check(!rcsdk::displayPosition(x, y, info));
    x = 0;
    check(rcsdk::displayPosition(x, y, info, true) && x == 0);
    x = 32768;
    y = 32768;
    check(rcsdk::displayPosition(x, y, info) && x > 32700 && x < 32840);
    // And back: a cursor's display position to the frame, then to the view.
    uint32_t fx = x, fy = y;
    rcsdk::framePosition(fx, fy, info);
    check(fx > 32700 && fx < 32840 && fy > 32700 && fy < 32840);
    fx = 0;
    fy = 0;
    rcsdk::framePosition(fx, fy, info); // The display's corner sits right of the letterbox.
    check(fx > 160 * 65535 / 1919 - 40 && fx < 160 * 65535 / 1919 + 40 && fy == 0);
    double px = 0, py = 0;
    check(fit.point(65535, 65535, 1920, 1080, px, py) && px > fit.x + fit.width - 2 && py > fit.y + fit.height - 2);
    check(fit.point(0, 0, 1920, 1080, px, py) && px == fit.x && py == fit.y);
    check(!crop.point(0, 0, 1920, 1080, px, py)); // Cropped away.
    // Shown one to one, every column maps to the nearest position, through the
    // frame and the display, and back to the same pixel.
    for (const uint32_t width : {1920u, 2560u, 3840u, 5120u}) {
        const auto view = rcsdk::viewGeometry(int(width), 1080, width, 1080, rcsdk::ViewScale::nativePixels);
        rcs_stream_info stream = RCS_STREAM_INFO_INIT;
        stream.width = stream.content_width = width;
        stream.height = stream.content_height = 1080;
        for (uint32_t column = 0; column < width; ++column) {
            check(view.position(column, 0, width, 1080, x, y) &&
                  x == (column * 65535 * 2 + width - 1) / (2 * (width - 1)));
            const uint32_t position = x;
            check(rcsdk::displayPosition(x, y, stream) && x == position);
            rcsdk::framePosition(x, y, stream);
            check(x == position && view.point(x, y, width, 1080, px, py) && std::lround(px) == long(column));
        }
    }
    info.display_width = 3200;
    check(rcsdk::displayScale(fit, info) > 0.26 && rcsdk::displayScale(fit, info) < 0.27); // 1000/1920 * 1600/3200
    return 0;
}
