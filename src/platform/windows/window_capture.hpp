// Capture of one window (RCS_CAPTURE_WINDOW) with Windows.Graphics.Capture.
#pragma once

#include "platform/platform.hpp"

#include <memory>

namespace rcsdk::platform::win {

// Windows 10 1903 or later: it can capture single windows.
bool windowCaptureAvailable() noexcept;
std::unique_ptr<ScreenCapture> createWindowCapture();

} // namespace rcsdk::platform::win
