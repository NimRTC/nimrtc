// modules/video_capture/src/platform/portable/portable_capture_stubs.cpp
// ----------------------------------------------------------------------------
// The "no Unix backends here" stub.
//
// Compiled on every platform that is neither macOS nor Linux (Windows,
// primarily).  It provides the three `detail::create_*` entry points the
// portable factory layer calls, and every one of them returns null --
// which makes the corresponding factory produce an "unsupported" device
// whose `open()` fails with `kErrUnsupported`.
//
// The point is that a host that names `"linux-pipewire"` on a Windows
// build gets a *clean* failure, not a link error and not a silent
// black screen.
// ----------------------------------------------------------------------------

#include "platform/unix_capture_factory.hpp"

#if !defined(__APPLE__) && !defined(__linux__)

namespace nimrtc::video_capture::detail {

plugins::IVideoCapture* create_screen_capture_kit(plugins::CaptureConfig) {
    return nullptr;
}

plugins::IVideoCapture* create_pipewire_capture(plugins::CaptureConfig) {
    return nullptr;
}

plugins::IVideoCapture* create_x11_capture(plugins::CaptureConfig) {
    return nullptr;
}

} // namespace nimrtc::video_capture::detail

#endif // !defined(__APPLE__) && !defined(__linux__)
