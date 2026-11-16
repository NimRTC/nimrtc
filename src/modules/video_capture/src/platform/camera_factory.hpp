// modules/video_capture/src/platform/camera_factory.hpp
// ----------------------------------------------------------------------------
// Internal declarations for the platform-specific camera adapters.
//
// NOT a public header: it lives under `src/`, not `include/`, and it is
// not installed.  It exists so that the portable factory layer
// (`src/camera_capture.cpp`) can hand the factory's `create()` over to
// the platform adapter without the public header having to name a
// concrete device type.
//
// Exactly one translation unit provides `create_camera_capture()` per
// build:
//
//   - Windows: src/platform/windows/mf_camera.cpp
//   - macOS:   src/platform/macos/avfoundation_camera.mm
//   - Linux:   src/platform/linux/v4l2_camera.cpp
//   - others:  src/platform/portable/portable_camera_stubs.cpp
//
// The function returns nullptr when the backend is not available in this
// build; the portable factory then produces an "unsupported" device that
// fails `open()` with `kErrUnsupported`.
// ----------------------------------------------------------------------------

#pragma once

#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/video_capture/camera_capture.hpp>

namespace nimrtc::video_capture::detail {

/**
 * @brief Create a camera capture device for the given backend.
 *
 * @param cfg     The requested configuration.
 * @param backend The backend to create the device for.
 * @return The device, or nullptr when the backend is unavailable in this
 *         build.
 */
plugins::IVideoCapture* create_camera_capture(plugins::CaptureConfig cfg,
                                               CameraBackend backend);

} // namespace nimrtc::video_capture::detail
