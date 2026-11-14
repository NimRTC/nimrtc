/**
 * @file nimrtc/video_capture/x11_capture.hpp
 * @brief The X11 capture device (remote-desktop plan A5).
 *
 * X11 is the *fallback* capture path on Linux.  It is the only one that
 * works without a desktop portal, and it is also the *slow* one: every
 * frame is a **round trip to the X server** (`XGetImage`), which means
 * the pixels are read back over a socket and copied into the process.
 * That is exactly the "software path" the plan's budget table prices at
 * ≤ 4 ms for 1080p — acceptable, but the Wayland path is the one that
 * should be preferred when it is available.
 *
 * ## Why X11 is still worth having
 *
 *   - **It works everywhere X11 works**, with no portal, no D-Bus, no
 *     PipeWire and no user interaction.  A headless X server (Xvfb),
 *     a remote X session over SSH (`ssh -X`), a kiosk built on X11 —
 *     all of them capture with this device and nothing else.
 *   - **It needs no consent.**  X11 has no screen-capture permission
 *     model, so there is nothing to ask for.
 *   - **It is the fallback the plan names.**  A5 says: "XDG Desktop
 *     Portal + PipeWire (Wayland) **and** X11 (`XGetImage`) fallback".
 *
 * ## What it produces
 *
 * A CPU-visible **BGRA** copy of the captured area, in the portable
 * layer's own format.  `XGetImage` returns the pixels in the server's
 * native format, so the backend converts on the way in: the plan's
 * portable layer speaks BGRA (the same format the Windows and macOS
 * backends produce), and the conversion is a per-pixel swizzle that
 * the CPU does at full speed.
 *
 * `XGetImage` also *does not* include the pointer, so this backend
 * reports `cursor_composited == false` and the cursor layer (plan A6)
 * draws the pointer — see `make_x11_cursor_source()`.
 *
 * ## Threading
 *
 * The device owns a **capture thread** (RD-6).  The X connection is
 * **not** thread-safe, so the connection is created *on the capture
 * thread* and every X call happens there.  The engine's tick thread
 * only ever pops from the shared bounded queue — it never touches the
 * X connection.
 *
 * The X connection is also closed from the capture thread (during
 * `close()`), for the same reason: closing a connection that another
 * thread is using is a use-after-free.
 *
 * @note Remote-desktop plan A5.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/video_capture/capture_source_adapter.hpp>
#include <nimrtc/video_capture/dirty_rect_diff.hpp>

namespace nimrtc::video_capture {

/**
 * @brief The X11 capture device.
 *
 * The class exists on every platform so that a host can refer to it
 * portably; on a non-Linux host `open()` always fails with
 * `kErrUnsupported`, and the factory is simply not registered there.
 */
class NIMRTC_API_STABLE X11Capture final : public plugins::IVideoCapture {
public:
    explicit X11Capture(plugins::CaptureConfig cfg) noexcept;
    ~X11Capture() override;

    X11Capture(const X11Capture&)            = delete;
    X11Capture& operator=(const X11Capture&) = delete;

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override { return "x11-capture"; }

    plugins::Status open() noexcept override;
    void close() noexcept override;

    // ---- IVideoCapture ----------------------------------------------------
    [[nodiscard]] std::vector<plugins::CaptureDevice>
    enumerate_devices() const noexcept override;

    [[nodiscard]] std::vector<plugins::CaptureFormat>
    supported_formats(const std::string& device_id) const noexcept override;

    [[nodiscard]] plugins::CaptureCapabilities
    capabilities() const noexcept override;

    [[nodiscard]] bool is_open() const noexcept override { return open_; }

    [[nodiscard]] plugins::CaptureConfig config() const noexcept override {
        return cfg_;
    }

    plugins::Status pop_frame(plugins::CaptureFrame& out) noexcept override;

    void set_frame_callback(plugins::CaptureFrameCallback cb) noexcept override {
        cb_ = std::move(cb);
    }

    [[nodiscard]] plugins::CaptureStats stats() const noexcept override;

    // ---- X11-specific -----------------------------------------------------

    /** True iff this build can talk to X11 at all.  False on a
     *  non-Linux host (and on a Linux host built without X11). */
    [[nodiscard]] static bool available() noexcept;

    /** The captured display's description, for diagnostics. */
    [[nodiscard]] std::string device_description() const noexcept {
        return description_;
    }

private:
    /** The capture thread's body: grab, convert, queue. */
    void capture_loop() noexcept;

    plugins::CaptureConfig cfg_;
    plugins::CaptureFrameCallback cb_;

    /** The counters.  They are written by the capture thread and read
     *  by the engine's tick thread, so they are atomic. */
    std::atomic<std::uint64_t> frames_captured_{0};
    std::atomic<std::uint64_t> frames_dropped_{0};
    std::atomic<std::uint64_t> device_errors_{0};
    std::atomic<std::uint64_t> bytes_copied_{0};

    /** The pixel buffer the capture thread fills (the whole screen). */
    std::vector<std::uint8_t> scratch_;

    /** The cropped buffer, used when a sub-region was requested. */
    std::vector<std::uint8_t> region_scratch_;

    /** The changed-region detector (remote-desktop plan A7).  X11 reports
     *  no damage, so the region is computed by comparing the frame with
     *  the previous one. */
    DirtyRectDetector dirty_detector_{4};

    /** The human-readable description of the captured display. */
    std::string description_;

    /** The X11 objects and the capture thread. */
    std::unique_ptr<class X11CaptureState> state_;

    /** The frame sequence counter.  Only the capture thread touches
     *  it. */
    std::uint32_t frame_index_ = 0;
    bool open_ = false;
};

// ---------------------------------------------------------------------------
// The factory
// ---------------------------------------------------------------------------

/**
 * @brief The X11 capture factory.
 *
 * Registers under the id `"x11"`.  On a non-Linux host (or a Linux
 * build without X11) the factory is never registered, so a host that
 * configures `"x11"` there gets the engine's "capture plugin missing"
 * error rather than a silent no-op.
 */
class NIMRTC_API_STABLE X11CaptureFactory final
    : public plugins::IVideoCaptureFactory {
public:
    [[nodiscard]] std::string_view id() const noexcept override {
        return "x11";
    }

    [[nodiscard]] std::string_view display_name() const noexcept override {
        return "X11 (XGetImage display capture)";
    }

    [[nodiscard]] plugins::IVideoCapture* create(
        plugins::CaptureConfig cfg) const override {
        return new X11Capture(std::move(cfg));
    }

    [[nodiscard]] plugins::IVideoSource* create_source(
        plugins::IVideoCapture* device,
        plugins::VideoSourceConfig cfg) const override {
        return make_capture_source(device, std::move(cfg));
    }
};

/**
 * @brief Publish the X11 factory into the plugin registry.
 *
 * A no-op on a non-Linux host (and on a Linux build without X11).
 * Called by the module's `register_default_plugins()`; it is separate
 * so that the *platform* file owns the platform's registration and the
 * portable file stays portable.
 */
NIMRTC_API_STABLE void register_x11_capture() noexcept;

} // namespace nimrtc::video_capture
