// modules/video_capture/src/platform/macos/screencapturekit_capture.hpp
// ----------------------------------------------------------------------------
// The macOS ScreenCaptureKit capture device (remote-desktop plan A4).
//
// INTERNAL header -- it lives under `src/`, not `include/`, and it is
// not installed.  It is the thin adapter over `UnixCaptureDevice`:
//
//   - it asks ScreenCaptureKit for the shareable content (the displays
//     and windows the user may capture),
//   - it configures an `SCStream` and starts it,
//   - and it hands each `CMSampleBuffer`'s `CVPixelBuffer` to the
//     portable core.
//
// Everything else -- the queue, the drop policy, the dirty-rect
// bookkeeping, the statistics, the negotiation -- comes from the
// portable core.
//
// ## The zero-copy contract (A10 / B10)
//
// ScreenCaptureKit delivers a `CVPixelBuffer`, which is a native GPU
// surface.  It is handed to the core as a `GpuBuffer`-style handle with
// `GpuBackend::kAppleCFPixelBuffer`, so the pixels never enter host
// memory on the fast path.  When the caller wants a CPU frame (or the
// fast path is not usable), the buffer is locked and copied, and the
// copy is counted in `CaptureStats::bytes_copied`.
//
// ## The permission path
//
// macOS asks for screen-recording permission through
// `CGPreflightScreenCaptureAccess()` / `CGRequestScreenCaptureAccess()`.
// A denial is a **clean error** with the distinct code
// `UnixCaptureError::kPermissionDenied` -- the device stops, the error
// callback fires, and nothing crashes.
//
// ScreenCaptureKit itself is macOS 12.3+.  On an older macOS the
// backend reports `kErrUnsupported` rather than pretending to work.
// ----------------------------------------------------------------------------

#pragma once

#if defined(__APPLE__)

#    include <cstdint>
#    include <memory>
#    include <nimrtc/plugins/video_capture.hpp>
#    include <nimrtc/video_capture/unix_capture.hpp>
#    include <string>
#    include <vector>

namespace nimrtc::video_capture {

/**
 * @brief The macOS ScreenCaptureKit capture device.
 *
 * Implements the six `platform_*` hooks of `UnixCaptureDevice`; the
 * portable half is inherited.
 */
class ScreenCaptureKitCapture final : public UnixCaptureDevice {
  public:
    explicit ScreenCaptureKitCapture(plugins::CaptureConfig cfg,
                                     UnixCaptureOptions opts = {}) noexcept;
    ~ScreenCaptureKitCapture() override;

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
        return "macos-screencapturekit-capture";
    }

    [[nodiscard]] std::string_view backend_id() const noexcept override {
        return "macos-screencapturekit";
    }

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nimrtc::video_capture

#endif // defined(__APPLE__)
