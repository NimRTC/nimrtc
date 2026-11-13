/**
 * @file nimrtc/video_capture/dxgi_desktop_dup.hpp
 * @brief The Windows DXGI Desktop Duplication capture device.
 *
 * DXGI Desktop Duplication is the display-capture path on Windows: the
 * OS gives the process a handle to the frame that the desktop
 * compositor is about to present, and the process reads it back.  It is
 * the *fast* path (a GPU-side copy, no window enumeration, no consent
 * prompt) and the *low-fidelity* one (the pointer is not in the frame,
 * and it is limited to the displays it can duplicate).
 *
 * The alternative on Windows is **Windows Graphics Capture** (WGC),
 * which composites the pointer and can capture a *window* — at the
 * cost of a user consent prompt.  The two are complementary:
 *
 *   - a *display* → DXGI (fast, no consent, no pointer)
 *   - a *window*  → WGC (consent, pointer composited)
 *
 * and the plan's A3 row names exactly that selection rule.
 *
 * ## What this backend produces
 *
 * A CPU-visible **BGRA** copy of the display, in the portable layer's
 * own format.  The desktop duplication API hands the frame over as a
 * GPU texture (`IDXGIResource`), so producing CPU pixels means a
 * GPU→CPU readback — that is the `bytes_copied` counter in the seam's
 * `CaptureStats`, and it is why `CaptureCapabilities::zero_copy` is
 * false for this backend.  The zero-copy path (the surface goes
 * straight to the hardware encoder without a readback) is the hardware
 * encoders' job (plan B10).
 *
 * The dirty rectangles come from DXGI itself
 * (`IDXGIOutputDuplication::GetFrameDirtyRects` /
 * `GetFrameMoveRects`), so the backend reports real per-frame changed
 * regions rather than a conservative bounding box.
 *
 * ## Threading
 *
 * The device owns a **capture thread** (RD-6 allows a capture plug-in
 * to own one; the engine stays a single-threaded orchestrator).  The
 * thread blocks in `AcquireNextFrame`, which is the natural place to
 * wait: the OS wakes it when the desktop changes.  The frames go into
 * the shared bounded queue, and the engine's tick thread only ever
 * calls `pop_frame()`, which takes an already-queued frame.
 *
 * This matters for latency: the tick loop must never park waiting for
 * a screen update, so the wait belongs on the capture thread.
 *
 * @note Remote-desktop plan A3.
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

namespace nimrtc::video_capture {

/**
 * @brief The DXGI Desktop Duplication capture device.
 *
 * The class exists on every platform so that a host can refer to it
 * portably; on a non-Windows host `open()` always fails with
 * `kErrUnsupported`, and the factory is simply not registered there.
 */
class NIMRTC_API_STABLE DxgiDesktopDupCapture final
    : public plugins::IVideoCapture {
public:
    explicit DxgiDesktopDupCapture(plugins::CaptureConfig cfg) noexcept;
    ~DxgiDesktopDupCapture() override;

    DxgiDesktopDupCapture(const DxgiDesktopDupCapture&)            = delete;
    DxgiDesktopDupCapture& operator=(const DxgiDesktopDupCapture&) = delete;

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override { return "dxgi-desktop-dup"; }

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

    // ---- DXGI-specific ----------------------------------------------------

    /** True iff this build can talk to DXGI at all.  False on a
     *  non-Windows host. */
    [[nodiscard]] static bool available() noexcept;

    /** The captured display's description, for diagnostics. */
    [[nodiscard]] std::string device_description() const noexcept {
        return description_;
    }

    // ---- The zero-copy path (remote-desktop plan B10) --------------------

    /**
     * @brief Take a free GPU surface from the pool.
     *
     * The zero-copy path copies the acquired desktop frame into a pooled
     * D3D11 texture with a *GPU-side* copy, so the surface outlives the
     * duplication frame and reaches the encoder without a CPU readback.
     *
     * @return The texture (an `ID3D11Texture2D*`), or null when the pool
     *         is exhausted.  The caller owns it until it hands it over.
     */
    [[nodiscard]] void* acquire_surface() noexcept;

    /**
     * @brief Return a retired surface to the pool.
     *
     * Called by the frame queue when it retires a surface (evicted by
     * the drop policy, or replaced by the next pop).  The producer may
     * then reuse the texture.
     */
    void release_surface(const video_frame::GpuHandle& surface) noexcept;

private:
    /** The capture thread's body: acquire, read back, queue. */
    void capture_loop() noexcept;

    plugins::CaptureConfig cfg_;
    plugins::CaptureFrameCallback cb_;

    /** The counters.  They are written by the capture thread and read
     *  by the engine's tick thread, so they are atomic. */
    std::atomic<std::uint64_t> frames_captured_{0};
    std::atomic<std::uint64_t> frames_dropped_{0};
    std::atomic<std::uint64_t> device_errors_{0};
    std::atomic<std::uint64_t> bytes_copied_{0};

    /** The pixel buffer the capture thread fills (the whole output). */
    std::vector<std::uint8_t> scratch_;

    /** The cropped buffer, used when a sub-region was requested. */
    std::vector<std::uint8_t> region_scratch_;

    /** The human-readable description of the captured display. */
    std::string description_;

    /** The DXGI objects and the capture thread. */
    std::unique_ptr<class DxgiDuplicationState> state_;

    /** The frame sequence counter.  Only the capture thread touches
     *  it. */
    std::uint32_t frame_index_ = 0;
    bool open_ = false;
};

// ---------------------------------------------------------------------------
// The factory
// ---------------------------------------------------------------------------

/**
 * @brief The DXGI Desktop Duplication factory.
 *
 * Registers under the id `"dxgi_desktop"`.  On a non-Windows host the
 * factory is never registered (there is no DXGI to talk to), so a host
 * that configures `"dxgi_desktop"` there gets the engine's
 * "capture plugin missing" error rather than a silent no-op.
 */
class NIMRTC_API_STABLE DxgiDesktopDupFactory final
    : public plugins::IVideoCaptureFactory {
public:
    [[nodiscard]] std::string_view id() const noexcept override {
        return "dxgi_desktop";
    }

    [[nodiscard]] std::string_view display_name() const noexcept override {
        return "Windows DXGI Desktop Duplication (display capture)";
    }

    [[nodiscard]] plugins::IVideoCapture* create(
        plugins::CaptureConfig cfg) const override {
        return new DxgiDesktopDupCapture(std::move(cfg));
    }
};

/**
 * @brief Publish the DXGI factory into the plugin registry.
 *
 * A no-op on a non-Windows host.  Called by the module's
 * `register_default_plugins()`; it is separate so that the *platform*
 * file owns the platform's registration and the portable file stays
 * portable.
 */
NIMRTC_API_STABLE void register_dxgi_desktop_dup() noexcept;

} // namespace nimrtc::video_capture
