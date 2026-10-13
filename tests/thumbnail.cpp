// Pictures without the screen: their size, the checks of what is asked for
// against a machine, the public calls' buffer contract (rcs_capture_picture,
// and the thumbnail module's rcs_thumbnail_capture through a session), with
// the picture taken through rcsdk::seams. tests/capture_targets.cpp takes real
// ones, tests/agent.cpp through an agent of another process.
#include "seams/seams.hpp"
#include "session/thumbnail.hpp"
#include "support/check.hpp"
#include "support/test_pattern.hpp"

#include <rcsdk/agent.hpp>
#include <rcsdk/rcsdk.hpp>
#include <rcsdk/thumbnail.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
#include <thread>

using namespace rcsdk;

namespace {

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

void sizes() {
    using platform::thumbnailSize;
    using Size = std::pair<uint32_t, uint32_t>;
    CHECK(thumbnailSize(1920, 1080, 320, 320) == Size(320, 180));
    CHECK(thumbnailSize(1080, 1920, 320, 320) == Size(180, 320));
    CHECK(thumbnailSize(5120, 2880, 400, 100) == Size(178, 100)); // The narrower limit decides, rounded.
    CHECK(thumbnailSize(100, 50, 1000, 1000) == Size(100, 50));   // Never enlarged...
    CHECK(thumbnailSize(320, 180, 320, 180) == Size(320, 180));   // ...nor changed when it fits.
    CHECK(thumbnailSize(8192, 1, 100, 100) == Size(100, 1));      // At least a pixel each way.
    CHECK(thumbnailSize(1, 8192, 100, 100) == Size(1, 100));
    CHECK(thumbnailSize(8192, 8192, 8192, 1) == Size(1, 1));
}

// The checks: those of a session's capture, against the machine that captures.
void checks() {
    agent::Machine machine;
    machine.capabilities.platform = RCS_PLATFORM_WINDOWS;
    machine.capabilities.capture_sources =
        (1u << RCS_CAPTURE_DISPLAY) | (1u << RCS_CAPTURE_REGION) | (1u << RCS_CAPTURE_APPLICATION);
    machine.screenPermission = RCS_PERMISSION_GRANTED;
    machine.displays = {{7, 1920, 1080, 60, 1}, {9, 1280, 1024, 75, 0}};
    int looked = 0;
    const auto machineNow = [&] {
        ++looked;
        return machine;
    };
    std::vector<platform::ThumbnailRequest> taken;
    const session::ThumbnailTaker take = [&](const platform::ThumbnailRequest& request) {
        taken.push_back(request);
        return platform::Thumbnail{2, 1, std::vector<uint8_t>(8, 1)};
    };
    const auto refusal = [&](const rcs_capture_config* what, uint32_t width = 64, uint32_t height = 64) {
        try {
            session::takeThumbnail(what, width, height, machineNow, take);
        } catch (const Failure& failure) {
            return std::pair<rcs_result, std::string>(failure.result(), failure.what());
        }
        return std::pair<rcs_result, std::string>(RCS_OK, "");
    };

    // What is asked for, as the machine takes it.
    rcs_capture_config what = RCS_CAPTURE_CONFIG_INIT;
    what.source = RCS_CAPTURE_REGION;
    what.display_id = 9;
    what.region_x = 100;
    what.region_y = 24;
    what.region_width = 640;
    what.region_height = 480;
    what.secure_desktop = 1;
    machine.capabilities.secure_desktop = 1;
    CHECK(refusal(&what, 300, 200).first == RCS_OK && taken.size() == 1);
    const platform::ThumbnailRequest region{{RCS_CAPTURE_REGION, 9, {100, 24, 640, 480}}, false, true, 300, 200};
    CHECK(taken.back() == region);
    what = RCS_CAPTURE_CONFIG_INIT;
    what.source = RCS_CAPTURE_APPLICATION;
    what.process_id = 4321;
    what.exclude_current_process = 1;
    CHECK(refusal(&what, 8192, 1).first == RCS_OK);
    const platform::ThumbnailRequest application{{RCS_CAPTURE_APPLICATION, 0, {}, 0, 4321}, true, false, 8192, 1};
    CHECK(taken.back() == application);

    // The machine's verdicts, as a session's start would give them.
    what = RCS_CAPTURE_CONFIG_INIT;
    what.source = RCS_CAPTURE_WINDOW;
    what.window_id = 77;
    auto refused = refusal(&what);
    CHECK(refused.first == RCS_ERROR_UNSUPPORTED && contains(refused.second, "single windows")); // Not offered here.
    what = RCS_CAPTURE_CONFIG_INIT;
    what.display_id = 5;
    refused = refusal(&what);
    CHECK(refused.first == RCS_ERROR_INVALID_ARGUMENT && contains(refused.second, "display 5"));
    what.display_id = 9;
    what.source = RCS_CAPTURE_REGION;
    what.region_x = 1200;
    what.region_width = what.region_height = 100;
    refused = refusal(&what);
    CHECK(refused.first == RCS_ERROR_INVALID_ARGUMENT && contains(refused.second, "not inside display 9"));
    what = RCS_CAPTURE_CONFIG_INIT;
    what.secure_desktop = 1;
    machine.capabilities.secure_desktop = 0;
    CHECK(refusal(&what).first == RCS_ERROR_UNSUPPORTED);
    what.secure_desktop = 0;
    machine.screenPermission = RCS_PERMISSION_REQUIRED;
    CHECK(refusal(&what).first == RCS_ERROR_PERMISSION_REQUIRED);
    machine.screenPermission = RCS_PERMISSION_GRANTED;

    // Arguments no version takes, refused before the machine is asked.
    const size_t asked = taken.size();
    const int lookedBefore = looked;
    CHECK(refusal(nullptr).first == RCS_ERROR_INVALID_ARGUMENT);
    for (const auto [width, height] : {std::pair<uint32_t, uint32_t>{0, 64}, {64, 0}, {8193, 64}, {64, 8193}}) {
        refused = refusal(&what, width, height);
        CHECK(refused.first == RCS_ERROR_INVALID_ARGUMENT && contains(refused.second, "1..8192"));
    }
    for (const rcs_capture_source source : {RCS_CAPTURE_HOST, RCS_CAPTURE_ENCODED}) {
        what.source = source;
        refused = refusal(&what);
        CHECK(refused.first == RCS_ERROR_INVALID_ARGUMENT && contains(refused.second, "HOST and ENCODED"));
    }
    what = RCS_CAPTURE_CONFIG_INIT;
    rcs_log_config other = RCS_LOG_CONFIG_INIT;
    what.header.next = &other.header; // Alone, or not at all.
    CHECK(refusal(&what).first == RCS_ERROR_INVALID_ARGUMENT);
    what.header.next = nullptr;
    what.header.struct_size -= 4;
    CHECK(refusal(&what).first == RCS_ERROR_INVALID_ARGUMENT);
    what = RCS_CAPTURE_CONFIG_INIT;
    what.header.type = RCS_EXT_LOG;
    CHECK(refusal(&what).first == RCS_ERROR_INVALID_ARGUMENT);
    what = RCS_CAPTURE_CONFIG_INIT;
    what.source = 9;
    CHECK(refusal(&what).first == RCS_ERROR_INVALID_ARGUMENT);
    what = RCS_CAPTURE_CONFIG_INIT;
    what.source = RCS_CAPTURE_WINDOW; // No window_id.
    CHECK(refusal(&what).first == RCS_ERROR_INVALID_ARGUMENT);
    what = RCS_CAPTURE_CONFIG_INIT;
    what.source = RCS_CAPTURE_APPLICATION;
    what.process_id = 4;
    what.secure_desktop = 1; // Windows show on the user's desktop only.
    CHECK(refusal(&what).first == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(taken.size() == asked && looked == lookedBefore);
}

// The public calls: the buffer is the caller's; a picture that does not fit
// says how large it is and leaves the buffer alone.
void publicCalls() {
    test::UseTestPattern pattern; // The screen's checks pass: the pictures are the seam's.
    std::vector<platform::ThumbnailRequest> asked;
    seams::setThumbnailCapture([&](const platform::ThumbnailRequest& request) {
        asked.push_back(request);
        if (request.target.source == RCS_CAPTURE_WINDOW) {
            fail(RCS_ERROR_INVALID_STATE, "The window does not show: it is minimized (test)");
        }
        const auto [width, height] = platform::thumbnailSize(1600, 900, request.maxWidth, request.maxHeight);
        platform::Thumbnail picture{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
        for (size_t i = 0; i < picture.pixels.size(); ++i) {
            picture.pixels[i] = uint8_t(i % 251);
        }
        return picture;
    });
    const auto pixelsOk = [](const uint8_t* data, uint64_t size) {
        for (uint64_t i = 0; i < size; ++i) {
            if (data[i] != uint8_t(i % 251)) {
                return false;
            }
        }
        return true;
    };

    rcs_capture_config what = RCS_CAPTURE_CONFIG_INIT;
    std::vector<uint8_t> buffer(160 * 90 * 4, 0xEE);
    rcs_picture out = RCS_PICTURE_INIT;
    out.data = buffer.data();
    out.capacity = buffer.size() - 1; // One byte short.
    CHECK(rcs_capture_picture(&what, 160, 160, &out) == RCS_ERROR_BUFFER_TOO_SMALL);
    CHECK(out.width == 160 && out.height == 90 && out.stride_bytes == 640 && out.format == RCS_PIXEL_BGRA8 &&
          out.data_size == buffer.size());
    CHECK(std::all_of(buffer.begin(), buffer.end(), [](uint8_t b) { return b == 0xEE; }));
    char reason[256] = {};
    rcs_get_last_call_error(reason, sizeof(reason));
    CHECK(contains(reason, "57600 bytes"));
    out.capacity = buffer.size();
    CHECK(rcs_capture_picture(&what, 160, 160, &out) == RCS_OK);
    CHECK(out.width == 160 && out.height == 90 && out.data_size == buffer.size() && pixelsOk(buffer.data(), 57600));
    CHECK(asked.back() == (platform::ThumbnailRequest{{RCS_CAPTURE_DISPLAY}, false, false, 160, 160}));
    rcs_picture sizeOnly = RCS_PICTURE_INIT; // No buffer: the size it takes.
    CHECK(rcs_capture_picture(&what, 4000, 4000, &sizeOnly) == RCS_ERROR_BUFFER_TOO_SMALL && sizeOnly.width == 1600 &&
          sizeOnly.data_size == 1600u * 900 * 4);

    // Why there is no picture, as the machine said it.
    what.source = RCS_CAPTURE_WINDOW;
    what.window_id = 3;
    CHECK(rcs_capture_picture(&what, 160, 160, &out) == RCS_ERROR_INVALID_STATE);
    rcs_get_last_call_error(reason, sizeof(reason));
    CHECK(contains(reason, "minimized"));

    // The output structure is checked like any other.
    what = RCS_CAPTURE_CONFIG_INIT;
    rcs_picture wrong = out;
    wrong.struct_size = 8;
    CHECK(rcs_capture_picture(&what, 160, 160, &wrong) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(rcs_capture_picture(&what, 160, 160, nullptr) == RCS_ERROR_INVALID_ARGUMENT);
    wrong = out;
    wrong.data = nullptr; // A capacity without a buffer.
    CHECK(rcs_capture_picture(&what, 160, 160, &wrong) == RCS_ERROR_INVALID_ARGUMENT);
    CHECK(rcs_capture_picture(nullptr, 160, 160, &out) == RCS_ERROR_INVALID_ARGUMENT);

    // The C++ wrapper: a vector of the picture's size, from a chained extension too.
    auto config =
        rcsdk::SessionConfig(RCS_ROLE_SHARE).setControl(true).setCaptureRegion(0, 0, 0, 800, 600);
    config.get(); // Chains the extensions: capture()'s next is set now.
    CHECK(config.capture().header.next != nullptr);
    const auto picture = rcsdk::capturePicture(config.capture(), 400, 400);
    CHECK(picture.width == 400 && picture.height == 225 && picture.pixels.size() == 400u * 225 * 4 &&
          pixelsOk(picture.pixels.data(), picture.pixels.size()));
    CHECK(asked.back().target.source == RCS_CAPTURE_REGION);
    what.source = RCS_CAPTURE_WINDOW;
    what.window_id = 3;
    try {
        rcsdk::capturePicture(what, 100, 100);
        CHECK(false);
    } catch (const rcsdk::Error& error) {
        CHECK(error.result() == RCS_ERROR_INVALID_STATE && contains(error.what(), "minimized"));
    }

    // Through a session that uses the thumbnail module: taken where its
    // machine's half runs, here the session's own process once it runs.
    CHECK(rcs_register_module(rcs_thumbnail_module()) == RCS_OK);
    const auto capture = rcsdk::defaults<rcs_capture_config>();
    const auto take = [&](rcs_session session, rcs_picture& picture) {
        return rcs_thumbnail_capture(session, &capture, 32, 32, &picture);
    };
    std::vector<uint8_t> frame(32 * 18 * 4);
    rcs_picture into = RCS_PICTURE_INIT;
    into.data = frame.data();
    into.capacity = frame.size();
    const auto caps = rcsdk::capabilities();
    if (!((caps.hardware_encoders | caps.software_encoders) & (1u << RCS_CODEC_H264)) ||
        !(caps.capture_sources & (1u << RCS_CAPTURE_DISPLAY))) {
        std::printf("No display sharing; thumbnails of a running session are not tested\n");
        seams::setThumbnailCapture({});
        return;
    }
    rcsdk::SessionConfig share(RCS_ROLE_SHARE);
    share.setAddress("127.0.0.1", 47971).setKey(rcsdk::generateConnectionKey());
    {
        rcsdk::Session session(share); // No module.
        CHECK(take(session.handle(), into) == RCS_ERROR_INVALID_STATE);
    }
    share.useModule(RCS_THUMBNAIL_MODULE, nullptr, 0);
    {
        rcsdk::Session session(share);
        CHECK(take(session.handle(), into) == RCS_ERROR_INVALID_STATE); // Not running.
        CHECK(contains(rcsdk::lastCallError(), "not running"));
        CHECK(session.start() == RCS_OK);
        rcs_result result = RCS_ERROR_INVALID_STATE;
        for (int i = 0; i < 100 && result == RCS_ERROR_INVALID_STATE; ++i) { // Its machine's half starts.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            result = take(session.handle(), into);
        }
        CHECK(result == RCS_OK && into.width == 32 && into.height == 18 && pixelsOk(frame.data(), frame.size()));
        CHECK(asked.back() == (platform::ThumbnailRequest{{RCS_CAPTURE_DISPLAY}, false, false, 32, 32}));
        // Why there is none comes from the machine; so does a picture too large for the buffer.
        auto window = capture;
        window.source = RCS_CAPTURE_WINDOW;
        window.window_id = 3;
        CHECK(rcs_thumbnail_capture(session.handle(), &window, 32, 32, &into) == RCS_ERROR_INVALID_STATE);
        CHECK(contains(rcsdk::lastCallError(), "minimized"));
        into.capacity = frame.size() - 1;
        CHECK(take(session.handle(), into) == RCS_ERROR_BUFFER_TOO_SMALL && into.data_size == frame.size());
        into.capacity = frame.size();
        // What is refused before anything is asked.
        CHECK(rcs_thumbnail_capture(session.handle(), &capture, 0, 32, &into) == RCS_ERROR_INVALID_ARGUMENT);
        CHECK(rcs_thumbnail_capture(session.handle(), nullptr, 32, 32, &into) == RCS_ERROR_INVALID_ARGUMENT);
        CHECK(rcs_thumbnail_capture(nullptr, &capture, 32, 32, &into) == RCS_ERROR_INVALID_ARGUMENT);
        rcs_picture unsized = into;
        unsized.struct_size = 8;
        CHECK(take(session.handle(), unsized) == RCS_ERROR_INVALID_ARGUMENT);
        session.stop();
        CHECK(take(session.handle(), into) == RCS_ERROR_INVALID_STATE);
    }
    {
        // A viewer has nothing to share: the module takes no part there.
        rcsdk::SessionConfig view(RCS_ROLE_VIEW);
        view.setAddress("127.0.0.1", 47971).setKey(rcsdk::generateConnectionKey());
        view.useModule(RCS_THUMBNAIL_MODULE, nullptr, 0);
        rcs_session session = nullptr;
        CHECK(rcs_session_create(&view.get(), nullptr, &session) == RCS_ERROR_INVALID_ARGUMENT && !session);
    }
    {
        // An agent of another process: none attached, nothing to take it.
        share.use(rcsdk::agentLink(
#ifdef _WIN32
            "rcsdk-test-thumbnail",
#elif defined(__ANDROID__) // No /tmp: where the test runner says (TMPDIR).
            std::getenv("TMPDIR") + std::string("/rcsdk-test-thumbnail.sock"),
#else
            "/tmp/rcsdk-test-thumbnail.sock",
#endif
            rcsdk::generateConnectionKey()));
        rcsdk::Session session(share);
        CHECK(session.start() == RCS_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(take(session.handle(), into) == RCS_ERROR_INVALID_STATE);
        CHECK(contains(rcsdk::lastCallError(), "does not run on the machine"));
    }
    seams::setThumbnailCapture({});
}

} // namespace

int main() {
    return runTest("thumbnail", [] {
        sizes();
        checks();
        publicCalls();
    });
}
