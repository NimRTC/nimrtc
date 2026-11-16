// modules/video_capture/src/platform/linux/x11_capture.hpp
// ----------------------------------------------------------------------------
// The Linux X11 capture device (remote-desktop plan A5, the fallback).
//
// INTERNAL header -- it lives under `src/`, not `include/`, and it is
// not installed.
//
// The X11 path is the *fallback* for a Linux host that is not running
// Wayland.  It captures the root window (or a selected window) with
// `XGetImage`, which produces a `ZPixmap`; the adapter converts it to
// the BGRA the pipeline wants and hands it to the portable core.
//
// The device-level answer for dirty rectangles is **false**: X11 has no
// native "what changed" information, so the *portable pixel-diff* in
// `UnixCaptureCore` supplies the rectangles instead.  That is exactly
// what plan item A7 asks for ("macOS / Linux supply a conservative
// bounding box from a pixel-diff").
// ----------------------------------------------------------------------------

#pragma once

#if defined(__linux__)

#    include <cstdint>
#    include <memory>
#    include <nimrtc/plugins/video_capture.hpp>
#    include <nimrtc/video_capture/unix_capture.hpp>
#    include <string>
#    include <vector>

namespace nimrtc::video_capture {

/**
 * @brief The Linux X11 capture device.
 *
 * Implements the six `platform_*` hooks of `UnixCaptureDevice`; the
 * portable half is inherited.
 */
class X11Capture final : public UnixCaptureDevice {
  public:
    explicit X11Capture(plugins::CaptureConfig cfg, UnixCaptureOptions opts = {}) noexcept;
    ~X11Capture() override;

  protected:
    [[nodiscard]] std::vector<plugins::CaptureDevice>
    platform_enumerate_devices() const noexcept override;

    [[nodiscard]] std::vector<plugins::CaptureFormat>
    platform_supported_formats(const std::string& device_id) const noexcept override;

    plugins::Status platform_open_device(UnixCaptureError& error) noexcept override;
    void platform_close_device() noexcept override;

    [[nodiscard]] plugins::CaptureCapabilities platform_capabilities() const noexcept override;

    void platform_start() noexcept override;
    void platform_stop() noexcept override;

    plugins::Status platform_poll_frame() noexcept override;

    [[nodiscard]] std::string_view plugin_name() const noexcept override {
        return "linux-x11-capture";
    }

    [[nodiscard]] std::string_view backend_id() const noexcept override { return "linux-x11"; }

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nimrtc::video_capture

#endif // defined(__linux__)
