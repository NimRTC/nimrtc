// modules/video_capture/src/platform/unix_capture_factory.hpp
// ----------------------------------------------------------------------------
// Internal declarations for the platform-specific capture adapters.
//
// NOT a public header: it lives under `src/`, not `include/`, and it is
// not installed.  It exists so that the portable base class
// (`src/unix_capture.cpp`) can hand the factory's `create()` over to the
// platform adapter without the public header having to name a concrete
// device type.
//
// Each platform adapter defines *all three* of these functions, so
// exactly one translation unit provides them per build:
//
//   - macOS  : src/platform/macos/macos_capture.cpp
//   - Linux  : src/platform/linux/linux_capture.cpp
//   - others : src/platform/portable/portable_capture_stubs.cpp
//
// A function returns nullptr when the backend is not available in this
// build; the portable factory then produces an "unsupported" device that
// fails `open()` with `kErrUnsupported`.
// ----------------------------------------------------------------------------

#pragma once

#include <nimrtc/plugins/video_capture.hpp>

namespace nimrtc::video_capture::detail {

/** Create a macOS ScreenCaptureKit capture device (nullptr when the
 *  backend is unavailable). */
plugins::IVideoCapture* create_screen_capture_kit(plugins::CaptureConfig cfg);

/** Create a Linux PipeWire (portal) capture device (nullptr when the
 *  backend is unavailable). */
plugins::IVideoCapture* create_pipewire_capture(plugins::CaptureConfig cfg);

/** Create a Linux X11 capture device (nullptr when the backend is
 *  unavailable). */
plugins::IVideoCapture* create_x11_capture(plugins::CaptureConfig cfg);

} // namespace nimrtc::video_capture::detail
