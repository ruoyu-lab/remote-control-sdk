// A screen capture replacement producing a moving test pattern in CPU memory,
// installed through rcsdk::seams::setCaptureFactory.
#pragma once

#include "platform/platform.hpp"
#include "seams/seams.hpp"

#include <atomic>
#include <thread>
#include <vector>

namespace rcsdk::test {

// Changes a test makes to the running test pattern.
struct PatternControl {
    // Like a lock screen: no frames, and paused() says this.
    std::atomic<rcs_capture_pause> paused{RCS_CAPTURE_PAUSE_NONE};
    std::atomic<uint64_t> interruptions{0};
    // Nonzero: the display fills only this width, centred (as after a display mode change).
    std::atomic<uint32_t> contentWidth{0};
    // Fine grain over a gradient, new every frame: the encoder uses its whole bitrate.
    std::atomic<bool> grain{false};
    // The display the pattern stands for (stream policy limits come from it).
    std::atomic<uint32_t> displayWidth{1920}, displayHeight{1080}, displayRefresh{60};
    // Set by the sharer: whether the pointer is drawn into the pictures, and
    // whether capture follows secure desktops.
    std::atomic<bool> cursorInVideo{true};
    std::atomic<bool> secureDesktop{false};
    // The pointer: hotspot 0..65535 across the display; the image switches
    // between two when `cursorImage` changes (1 or 2).
    std::atomic<uint32_t> cursorX{0}, cursorY{0}, cursorImage{1};
    // The next capture fails to start (an agent's own failure).
    std::atomic<bool> failStart{false};
    // Each capture takes this long to start, as a real one (or an encoder) may.
    std::atomic<uint32_t> startMs{0};
};

inline PatternControl& patternControl() {
    static PatternControl control;
    return control;
}

class TestPatternCapture final : public platform::ScreenCapture {
public:
    void start(const platform::CaptureConfig& config, CancelToken) override {
        if (patternControl().failStart.exchange(false)) {
            fail(RCS_ERROR_CAPTURE, "The test pattern cannot start (test)");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(patternControl().startMs.load()));
        config_ = config;
        next_ = Clock::now();
        patternControl().cursorInVideo = config.cursorInVideo;
        patternControl().secureDesktop = config.secureDesktop;
    }
    void setCursorInVideo(bool inVideo) override { patternControl().cursorInVideo = inVideo; }
    CursorState cursor() const override {
        auto& control = patternControl();
        CursorState state;
        state.visible = true;
        state.x = control.cursorX;
        state.y = control.cursorY;
        state.imageSerial = control.cursorImage;
        auto image = std::make_shared<CursorImage>();
        image->type = control.cursorImage == 1 ? RCS_CURSOR_ARROW : RCS_CURSOR_HAND;
        image->width = image->height = 16;
        image->hotspotX = control.cursorImage == 1 ? 0 : 6;
        image->pixels.assign(16 * 16 * 4, 0);
        for (size_t i = 0; i < image->pixels.size(); i += 4) { // A filled triangle, opaque, in one colour per image.
            const size_t x = i / 4 % 16, y = i / 4 / 16;
            if (x <= y) {
                image->pixels[i] = control.cursorImage == 1 ? 255 : 0;
                image->pixels[i + 2] = control.cursorImage == 1 ? 0 : 255;
                image->pixels[i + 3] = 255;
            }
        }
        state.image = std::move(image);
        return state;
    }
    platform::DisplayMode display() const noexcept override {
        auto& control = patternControl();
        return {control.displayWidth, control.displayHeight, control.displayRefresh};
    }

    bool next(VideoFrame& frame, std::chrono::milliseconds timeout, CancelToken cancelled) override {
        const auto deadline = Clock::now() + timeout;
        if (patternControl().paused) {
            while (patternControl().paused && !cancelled && Clock::now() < deadline) {
                std::this_thread::sleep_for(1ms);
            }
            return false;
        }
        while (Clock::now() < next_) {
            if (cancelled || Clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        next_ += std::chrono::nanoseconds(1'000'000'000 / config_.framesPerSecond);
        auto pixels = std::make_shared<std::vector<uint8_t>>(size_t(config_.width) * config_.height * 4, 40);
        if (patternControl().grain) {
            uint32_t state = uint32_t(frames_ * 2654435761u) | 1;
            for (size_t i = 0; i < pixels->size(); i += 4) {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                (*pixels)[i] = uint8_t(i / 4 % config_.width + (state & 31));
                (*pixels)[i + 1] = uint8_t(i / 4 / config_.width + (state >> 8 & 31));
                (*pixels)[i + 2] = uint8_t(96 + (state >> 16 & 31));
            }
        }
        const uint32_t column = uint32_t(frames_ * 7 % config_.width);
        for (uint32_t y = 0; y < config_.height; ++y) {
            for (uint32_t x = column; x < std::min(config_.width, column + 16); ++x) {
                uint8_t* p = pixels->data() + (size_t(y) * config_.width + x) * 4;
                p[0] = p[1] = p[2] = 230;
            }
        }
        frame.width = config_.width;
        frame.height = config_.height;
        frame.stride = config_.width * 4;
        frame.pixels = std::move(pixels);
        frame.sequence = ++frames_;
        frame.timestampNs = monotonicNs();
        return true;
    }

    void stop() noexcept override {}
    uint64_t captured() const noexcept override { return frames_; }
    uint64_t overwritten() const noexcept override { return 0; }
    rcs_capture_pause paused() const noexcept override { return patternControl().paused; }
    uint64_t interruptions() const noexcept override { return patternControl().interruptions; }
    Rect content() const noexcept override {
        const uint32_t width = patternControl().contentWidth ? patternControl().contentWidth.load() : config_.width;
        return {(config_.width - width) / 2, 0, width, config_.height};
    }

private:
    platform::CaptureConfig config_;
    Clock::time_point next_;
    uint64_t frames_ = 0;
};

// Installs the test pattern for the lifetime of this object.
struct UseTestPattern {
    UseTestPattern() {
        patternControl().paused = RCS_CAPTURE_PAUSE_NONE;
        patternControl().interruptions = 0;
        patternControl().contentWidth = 0;
        patternControl().displayWidth = 1920;
        patternControl().displayHeight = 1080;
        patternControl().displayRefresh = 60;
        patternControl().cursorImage = 1;
        patternControl().secureDesktop = false;
        seams::setCaptureFactory([](const auto&) { return std::make_unique<TestPatternCapture>(); });
    }
    ~UseTestPattern() { seams::setCaptureFactory({}); }
};

} // namespace rcsdk::test
