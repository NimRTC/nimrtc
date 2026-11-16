/**
 * @file nimrtc/video_capture/unix_capture.hpp
 * @brief The macOS and Linux capture backends — thin adapters over the
 *        portable capture core.
 *
 * This is the landing of remote-desktop plan items **A4** (macOS
 * ScreenCaptureKit) and **A5** (Linux: XDG Desktop Portal + PipeWire
 * for Wayland, X11 as the fallback).  Both are *thin* adapters over the
 * mechanism that lives in `unix_capture_core.hpp`:
 *
 *   - the bounded `CaptureQueue` with its explicit drop policy,
 *   - the dirty-rectangle model (including the portable pixel-diff that
 *     supplies the rectangles the platforms do not report natively),
 *   - the format-negotiation contract,
 *   - the region crop,
 *   - and the capture statistics.
 *
 * Only the leaf OS calls differ per platform, exactly as plan item A2b
 * requires.
 *
 * ## What each backend does
 *
 *   - **ScreenCaptureKit** (`macos-screencapturekit`) captures a display
 *     or a window on macOS 12.3+.  It composites the pointer into the
 *     frame and produces a `CVPixelBuffer`, so the zero-copy path
 *     delivers a `GpuBuffer` with `GpuBackend::kAppleCFPixelBuffer`.
 *     Screen-recording permission is requested by the OS; a denial is
 *     a **clean error** with the distinct code
 *     `UnixCaptureError::kPermissionDenied` — never a crash and never
 *     an endless stream of black frames.
 *   - **PipeWire + portal** (`linux-pipewire`) captures a Wayland
 *     session.  The XDG Desktop Portal owns the source selection, so
 *     the device list is whatever the portal granted — the engine
 *     cannot enumerate monitors it was not given.  That is a platform
 *     limitation, not an engine bug, and the seam says so.
 *   - **X11** (`linux-x11`) captures the root window (or a selected
 *     window) with `XGetImage` and converts the `ZPixmap` to the BGRA
 *     the pipeline wants.  It reports `dirty_rects = false` at the
 *     device level: the portable pixel-diff supplies the rectangles
 *     instead.
 *
 * When `WAYLAND_DISPLAY` is set the Linux backend prefers the portal +
 * PipeWire path; when `DISPLAY` is set it falls back to X11.  When
 * neither is available the failure is explicit
 * (`UnixCaptureError::kDisplayUnavailable`).
 *
 * ## The threading rule (RD-6)
 *
 * The capture device owns its own thread.  It never blocks the engine's
 * tick thread on an OS call.  `UnixCaptureDevice` implements that: it
 * owns the thread, calls `platform_poll_frame()` in a loop, and feeds
 * the core.  The engine's tick thread only ever touches the bounded
 * queue.
 *
 * ## The zero-copy contract
 *
 * On the zero-copy path the frame is handed over *inside the callback*
 * as a GPU handle and the pixels never enter host memory.  When no GPU
 * consumer is attached — or the GPU path is not usable — the frame is
 * honestly **staged to the CPU**, and the copy is counted in
 * `CaptureStats::bytes_copied` so the caller can *see* that the fast
 * path was not taken.  Nothing is silently downgraded.
 *
 * @note Remote-desktop plan A4 / A5 / A7 / A8, building on A2b.
 */

#ifndef NIMRTC_VIDEO_CAPTURE_UNIX_CAPTURE_HPP
#define NIMRTC_VIDEO_CAPTURE_UNIX_CAPTURE_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <nimrtc/core/api_version.hpp>
#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/video_capture/unix_capture_core.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace nimrtc::video_capture {

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

/**
 * @brief Callback invoked when a capture device fails.
 *
 * The capture device stops after the callback returns; the host is
 * expected to re-open it (or to tear the session down).
 */
using NIMRTC_API_STABLE UnixCaptureErrorCallback =
    std::function<void(UnixCaptureError error, std::string_view message)>;

// ---------------------------------------------------------------------------
// UnixCaptureDevice
// ---------------------------------------------------------------------------

/**
 * @brief The base class of the macOS and Linux capture devices.
 *
 * It implements the whole of `plugins::IVideoCapture` in terms of
 * `UnixCaptureCore`, and leaves only the platform-specific parts
 * abstract:
 *
 *   - `platform_enumerate_devices()` — the device list.  On Wayland the
 *     portal owns this, so the list is what the portal granted.
 *   - `platform_supported_formats()` — the formats the device offers.
 *   - `platform_open_device()` / `platform_close_device()` — the
 *     lifecycle.
 *   - `platform_capabilities()` — what the backend can do.
 *   - `platform_start()` / `platform_stop()` — the capture thread's
 *     start and stop.
 *   - `platform_poll_frame()` — "give me the next frame".
 *
 * The base class owns the capture thread: it calls `platform_poll_frame()`
 * in a loop and feeds the core, so the engine's tick thread is never
 * parked by a slow OS call (RD-6).
 *
 * ## Writing a backend
 *
 * A backend is a subclass that implements the six `platform_*` hooks
 * and nothing else.  The queue, the sequence numbers, the statistics,
 * the dirty-rect bookkeeping and the negotiation all come from here.
 */
class NIMRTC_API_STABLE UnixCaptureDevice : public plugins::IVideoCapture {
  public:
    /** Construct a capture device. */
    explicit UnixCaptureDevice(plugins::CaptureConfig cfg, UnixCaptureOptions opts = {}) noexcept;

    ~UnixCaptureDevice() override;

    UnixCaptureDevice(const UnixCaptureDevice&) = delete;
    UnixCaptureDevice& operator=(const UnixCaptureDevice&) = delete;

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override;

    plugins::Status open() noexcept override;
    void close() noexcept override;

    // ---- IVideoCapture ----------------------------------------------------
    [[nodiscard]] std::vector<plugins::CaptureDevice> enumerate_devices() const noexcept override;

    [[nodiscard]] std::vector<plugins::CaptureFormat>
    supported_formats(const std::string& device_id) const noexcept override;

    [[nodiscard]] plugins::CaptureCapabilities capabilities() const noexcept override;

    [[nodiscard]] bool is_open() const noexcept override;

    [[nodiscard]] plugins::CaptureConfig config() const noexcept override;

    plugins::Status pop_frame(plugins::CaptureFrame& out) noexcept override;

    void set_frame_callback(plugins::CaptureFrameCallback cb) noexcept override;

    [[nodiscard]] plugins::CaptureStats stats() const noexcept override;

    // ---- Unix-specific ----------------------------------------------------

    /** Attach a failure callback (permission refused, device lost, ...). */
    void set_error_callback(UnixCaptureErrorCallback cb) noexcept;

    /** The last error the device reported. */
    [[nodiscard]] UnixCaptureError last_error() const noexcept;

    /**
     * @brief Re-open the device after a failure.
     *
     * The capture device stops itself when it hits an unrecoverable
     * error (permission refused, device lost).  A host that wants to
     * keep the session alive calls this; a host that does not simply
     * tears the session down.  Either way the device never crashes.
     */
    plugins::Status reopen() noexcept;

    /** The portable core this device drives. */
    [[nodiscard]] UnixCaptureCore& core() noexcept { return core_; }

    /** The portable core this device drives. */
    [[nodiscard]] const UnixCaptureCore& core() const noexcept { return core_; }

  protected:
    // ---- The platform hooks ------------------------------------------------

    /** The devices this backend can capture. */
    [[nodiscard]] virtual std::vector<plugins::CaptureDevice>
    platform_enumerate_devices() const noexcept = 0;

    /** The formats @p device_id can deliver. */
    [[nodiscard]] virtual std::vector<plugins::CaptureFormat>
    platform_supported_formats(const std::string& device_id) const noexcept = 0;

    /**
     * @brief Open the platform device.
     *
     * @param error Receives the failure code when the open fails.
     * @return kOk on success.
     */
    virtual plugins::Status platform_open_device(UnixCaptureError& error) noexcept = 0;

    /** Release the platform device. */
    virtual void platform_close_device() noexcept = 0;

    /** What this backend can do. */
    [[nodiscard]] virtual plugins::CaptureCapabilities platform_capabilities() const noexcept = 0;

    /** Start the platform's own producer (a ScreenCaptureKit stream, a
     *  PipeWire loop, an X11 event pump). */
    virtual void platform_start() noexcept {}

    /** Stop the platform's own producer. */
    virtual void platform_stop() noexcept {}

    /**
     * @brief Get the next frame from the platform.
     *
     * Called on the capture thread.  The implementation hands the frame
     * to the core (through `submit_frame()` / `submit_gpu_frame()`) and
     * returns.
     *
     * @return kOk when a frame was delivered, `kErrNotReady` when there
     *         is nothing to deliver right now (the base class then
     *         sleeps briefly rather than spinning), and any other status
     *         when the device failed (the base class stops the capture
     *         and reports the error).
     */
    virtual plugins::Status platform_poll_frame() noexcept = 0;

    /** The plugin name reported by `name()`. */
    [[nodiscard]] virtual std::string_view plugin_name() const noexcept = 0;

    /** The backend id reported by `capabilities().backend`. */
    [[nodiscard]] virtual std::string_view backend_id() const noexcept = 0;

    // ---- Helpers for the subclasses ---------------------------------------

    /** Report an error through the error callback and remember it. */
    void report_error(UnixCaptureError error, std::string_view message) noexcept;

    /** The configuration the device was constructed with. */
    [[nodiscard]] plugins::CaptureConfig requested_config() const noexcept {
        return core_.config();
    }

    /** The options the device was constructed with. */
    [[nodiscard]] UnixCaptureOptions capture_options() const noexcept { return core_.options(); }

    /** The negotiated format (set by the subclass after opening). */
    void set_negotiated_format(const plugins::CaptureFormat& format) noexcept {
        core_.set_negotiated_format(format);
    }

  private:
    void capture_loop() noexcept;
    void stop_thread() noexcept;

    UnixCaptureCore core_;
    UnixCaptureErrorCallback error_cb_;

    std::atomic<bool> running_{false};
    std::thread thread_;

    UnixCaptureError last_error_ = UnixCaptureError::kNone;
    bool open_ = false;
};

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------

/**
 * @brief Factory for the macOS ScreenCaptureKit capture device.
 *
 * Registered under the id `"macos-screencapturekit"`.
 */
class NIMRTC_API_STABLE ScreenCaptureKitFactory final : public plugins::IVideoCaptureFactory {
  public:
    [[nodiscard]] std::string_view id() const noexcept override { return "macos-screencapturekit"; }

    [[nodiscard]] std::string_view display_name() const noexcept override {
        return "macOS ScreenCaptureKit (display/window, cursor composited)";
    }

    [[nodiscard]] plugins::IVideoCapture* create(plugins::CaptureConfig cfg) const override;
};

/**
 * @brief Factory for the Linux PipeWire capture device.
 *
 * Registered under the id `"linux-pipewire"`.  The XDG Desktop Portal
 * owns the source selection, so the device list is whatever the portal
 * granted.
 */
class NIMRTC_API_STABLE PipeWireCaptureFactory final : public plugins::IVideoCaptureFactory {
  public:
    [[nodiscard]] std::string_view id() const noexcept override { return "linux-pipewire"; }

    [[nodiscard]] std::string_view display_name() const noexcept override {
        return "Linux PipeWire via the XDG Desktop Portal (Wayland)";
    }

    [[nodiscard]] plugins::IVideoCapture* create(plugins::CaptureConfig cfg) const override;
};

/**
 * @brief Factory for the Linux X11 capture device.
 *
 * Registered under the id `"linux-x11"`.
 */
class NIMRTC_API_STABLE X11CaptureFactory final : public plugins::IVideoCaptureFactory {
  public:
    [[nodiscard]] std::string_view id() const noexcept override { return "linux-x11"; }

    [[nodiscard]] std::string_view display_name() const noexcept override {
        return "Linux X11 capture (XGetImage, root window)";
    }

    [[nodiscard]] plugins::IVideoCapture* create(plugins::CaptureConfig cfg) const override;
};

/**
 * @brief Register the Unix capture factories with `core::PluginRegistry`.
 *
 * Registers whichever backends *this build* supports:
 *
 *   - on macOS: `"macos-screencapturekit"`,
 *   - on Linux: `"linux-pipewire"` and `"linux-x11"`.
 *
 * Idempotent.  On Windows this is a no-op, so the portable registration
 * path stays identical on every platform.
 */
NIMRTC_API_STABLE void register_unix_capture_plugins() noexcept;

/**
 * @brief Whether this build supports the macOS ScreenCaptureKit backend.
 *
 * False on every other platform, and false on macOS versions older
 * than 12.3 (where ScreenCaptureKit does not exist).
 */
NIMRTC_API_STABLE bool screen_capture_kit_available() noexcept;

/**
 * @brief Whether this build supports the Linux PipeWire backend.
 *
 * False when PipeWire was not found at configure time, or on a
 * non-Linux platform.
 */
NIMRTC_API_STABLE bool pipewire_capture_available() noexcept;

/**
 * @brief Whether this build supports the Linux X11 backend.
 *
 * False on a non-Linux platform, or when the X11 development headers
 * were not found at configure time.
 */
NIMRTC_API_STABLE bool x11_capture_available() noexcept;

} // namespace nimrtc::video_capture
#endif // NIMRTC_VIDEO_CAPTURE_UNIX_CAPTURE_HPP
