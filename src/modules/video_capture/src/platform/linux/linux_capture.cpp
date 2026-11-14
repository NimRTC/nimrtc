// modules/video_capture/src/platform/linux/linux_capture.cpp
// ----------------------------------------------------------------------------
// The Linux capture backends' factory glue (remote-desktop plan A5).
//
// It provides the `detail::create_*` entry points that the portable
// factory layer calls.  On Linux the X11 one is always real and the
// PipeWire one is real when `libpipewire` was found at configure time.
//
// ## The backend selection
//
// The portal + PipeWire path is the *preferred* one, because on a
// Wayland session it is the only one that works at all.  The X11 path
// is the fallback for an X11 session.
//
// A host picks the backend by name:
//
//   - `"linux-pipewire"` -- the Wayland path (portal),
//   - `"linux-x11"`      -- the X11 fallback.
//
// The engine can also ask which one fits the session: `WAYLAND_DISPLAY`
// set means PipeWire, `DISPLAY` set means X11, and neither means the
// capture cannot run at all.
// ----------------------------------------------------------------------------

#include "platform/unix_capture_factory.hpp"

#if defined(__linux__)

#    include "linux/pipewire_capture.hpp"
#    include "linux/x11_capture.hpp"

#    include <cstdlib>

namespace nimrtc::video_capture::detail {

plugins::IVideoCapture* create_screen_capture_kit(plugins::CaptureConfig) {
    // There is no ScreenCaptureKit on Linux.
    return nullptr;
}

plugins::IVideoCapture* create_pipewire_capture(plugins::CaptureConfig cfg) {
#    if defined(NIMRTC_HAS_PIPEWIRE_CAPTURE)
    return new PipeWireCapture(std::move(cfg));
#    else
    // PipeWire was not found at configure time: the backend reports
    // "unsupported" rather than failing the build.
    (void)cfg;
    return nullptr;
#    endif
}

plugins::IVideoCapture* create_x11_capture(plugins::CaptureConfig cfg) {
#    if defined(NIMRTC_HAS_X11_CAPTURE)
    return new X11Capture(std::move(cfg));
#    else
    (void)cfg;
    return nullptr;
#    endif
}

} // namespace nimrtc::video_capture::detail

#endif // defined(__linux__)
