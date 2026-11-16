// modules/video_capture/src/platform/linux/pipewire_capture.hpp
// ----------------------------------------------------------------------------
// The Linux PipeWire capture device (remote-desktop plan A5, the Wayland
// path).
//
// INTERNAL header -- it lives under `src/`, not `include/`, and it is
// not installed.
//
// ## Why the portal
//
// Wayland's security model means a client cannot see the screen on its
// own.  The **XDG Desktop Portal** (`org.freedesktop.portal.ScreenCast`)
// is the sanctioned way to ask: the *user* picks the source, and the
// portal hands back a PipeWire node.  The engine therefore cannot
// enumerate monitors it was not given -- that is a platform limitation,
// not an engine bug, and the seam says so (`CaptureCapabilities::enum
// eration` reflects what the portal granted).
//
// ## The flow
//
//   1. `CreateSession` on `org.freedesktop.portal.Desktop`.
//   2. `SelectSources` on the session, with `types = 1` (monitor) or
//      `types = 2` (window), and `multiple = false`.
//   3. `Start` on the session -- the portal responds with a PipeWire
//      file descriptor and the node id.
//   4. `libpipewire` connects to that node and delivers the frames.
//
// The frames come out as `SPA_VIDEO_FORMAT_BGRA` (the compositor's
// native format), so they are handed to the portable core as-is.
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
 * @brief The Linux PipeWire (portal) capture device.
 *
 * Implements the six `platform_*` hooks of `UnixCaptureDevice`; the
 * portable half is inherited.
 */
class PipeWireCapture final : public UnixCaptureDevice {
  public:
    explicit PipeWireCapture(plugins::CaptureConfig cfg, UnixCaptureOptions opts = {}) noexcept;
    ~PipeWireCapture() override;

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
        return "linux-pipewire-capture";
    }

    [[nodiscard]] std::string_view backend_id() const noexcept override { return "linux-pipewire"; }

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nimrtc::video_capture

#endif // defined(__linux__)
