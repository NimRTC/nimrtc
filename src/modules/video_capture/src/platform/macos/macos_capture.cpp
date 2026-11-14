// modules/video_capture/src/platform/macos/macos_capture.cpp
// ----------------------------------------------------------------------------
// The macOS capture backend's factory glue (remote-desktop plan A4).
//
// It provides the `detail::create_*` entry points that the portable
// factory layer calls.  On macOS the ScreenCaptureKit one is real; the
// Linux ones return null (this platform has no X11 or PipeWire).
//
// On every other platform this file is not compiled at all -- the
// portable stub file provides the same three symbols instead.
// ----------------------------------------------------------------------------

#include "platform/unix_capture_factory.hpp"

#if defined(__APPLE__)

#    include "macos/screencapturekit_capture.hpp"

namespace nimrtc::video_capture::detail {

plugins::IVideoCapture* create_screen_capture_kit(plugins::CaptureConfig cfg) {
    return new ScreenCaptureKitCapture(std::move(cfg));
}

plugins::IVideoCapture* create_pipewire_capture(plugins::CaptureConfig) {
    // There is no PipeWire on macOS.
    return nullptr;
}

plugins::IVideoCapture* create_x11_capture(plugins::CaptureConfig) {
    // There is no X11 on macOS.
    return nullptr;
}

} // namespace nimrtc::video_capture::detail

#endif // defined(__APPLE__)
