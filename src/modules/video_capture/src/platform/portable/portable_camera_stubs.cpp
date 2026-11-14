// modules/video_capture/src/platform/portable/portable_camera_stubs.cpp
// ----------------------------------------------------------------------------
// The "no camera backend here" stub.
//
// Compiled on every platform that has none of the camera backends (or
// on a Linux box without the V4L2 headers).  It provides the
// `detail::create_camera_capture()` entry point the portable factory
// layer calls, and it returns null -- which makes the corresponding
// factory produce an "unsupported" device whose `open()` fails with
// `kErrUnsupported`.
//
// The point is that a host that names `"camera-v4l2"` on a Windows
// build gets a *clean* failure, not a link error and not a silent
// black picture.
// ----------------------------------------------------------------------------

#include "platform/camera_factory.hpp"

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__linux__)

namespace nimrtc::video_capture::detail {

plugins::IVideoCapture* create_camera_capture(plugins::CaptureConfig, CameraBackend) {
    return nullptr;
}

} // namespace nimrtc::video_capture::detail

#endif // !defined(_WIN32) && !defined(__APPLE__) && !defined(__linux__)
