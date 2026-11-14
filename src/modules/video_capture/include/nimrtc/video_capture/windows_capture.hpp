/**
 * @file nimrtc/video_capture/windows_capture.hpp
 * @brief The Windows capture backends — DXGI Desktop Duplication + WGC.
 *
 * This is the M1-c (Windows-first) landing of remote-desktop plan item
 * **A3**: two *thin* adapters over the portable capture layer that
 * already exists in this module (the bounded `CaptureQueue`, the
 * dirty-rectangle model, the format-negotiation contract).
 *
 * ## Why two backends
 *
 * They are not interchangeable — they make different trade-offs:
 *
 *   - **DXGI Desktop Duplication** (`windows-dxgi`) captures a *display*.
 *     It is the cheapest path (no compositor round-trip), it reports the
 *     changed region natively through `GetFrameDirtyRects` /
 *     `GetFrameMoveRects`, and it does **not** composite the pointer —
 *     the cursor is a separate layer (plan item A6).
 *   - **Windows Graphics Capture** (`windows-wgc`) captures a *display*
 *     or a *window*.  The OS composites the pointer into the frame, so
 *     the picture is what the user sees; and it works per-window, which
 *     is what a remote-desktop session usually wants.  It requires the
 *     user's **consent** (`GraphicsCaptureAccess`), and it reports no
 *     dirty rectangles — the frame is a whole-frame picture.
 *
 * Both produce the same thing: a `video_frame::GpuBuffer` whose handle
 * is a `ID3D11Texture2D` in `B8G8R8A8_UNORM` (BGRA) — the format every
 * desktop compositor produces natively.
 *
 * ## The zero-copy contract (RD-6)
 *
 * The capture device owns its own thread.  It never blocks the engine's
 * tick thread on a device call.  When a GPU consumer is attached, the
 * frame is handed over *inside the callback* as a `GpuBuffer`:
 *
 * @code
 *   capture thread ──► GpuCaptureFrameCallback(GpuCaptureFrame&&)
 *                         └─► encoder consumes the texture
 *                             (the caller holds the reference until
 *                              the encode call returns)
 * @endcode
 *
 * The zero-copy path deliberately **does not** go through the
 * `CaptureQueue`: that queue *copies* the pixel bytes, which is exactly
 * what the zero-copy path exists to avoid.  When no GPU consumer is
 * attached (or when the GPU staging fails), the frame is honestly
 * **staged to the CPU** — copied into a `D3D11_USAGE_STAGING` texture,
 * mapped, and pushed through the `CaptureQueue` — and the copy is
 * counted in `CaptureStats::bytes_copied` so the caller can *see* that
 * the fast path was not taken.  Nothing is silently downgraded.
 *
 * ## Error handling
 *
 * Every failure mode gets its own error code (see
 * `WindowsCaptureError`), because "the user refused the capture
 * consent prompt" and "the GPU fell off the bus" call for very
 * different responses from the host.  A refused consent and a lost
 * device are reported through `on_error` and the capture stops
 * cleanly — it never crashes and never spins.
 *
 * @note Remote-desktop plan A3 / A6 / A7 / A8, milestone M1-c.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/video_capture/cursor_source.hpp>
#include <nimrtc/video_frame/frame.hpp>

namespace nimrtc::video_capture {

// ---------------------------------------------------------------------------
// Backend selection
// ---------------------------------------------------------------------------

/** Which Windows capture backend a device uses. */
enum class NIMRTC_API_STABLE WindowsCaptureBackend : std::uint8_t {
    /** DXGI Desktop Duplication — displays, dirty rects, no cursor. */
    kDxgiDesktopDuplication = 0,

    /** Windows Graphics Capture — displays or windows, cursor composited
     *  by the OS, requires the user's consent. */
    kWgc = 1,
};

/** Human-readable backend name (for logs / stats). */
NIMRTC_API_STABLE inline std::string_view windows_capture_backend_name(
        WindowsCaptureBackend b) noexcept {
    switch (b) {
        case WindowsCaptureBackend::kDxgiDesktopDuplication:
            return "windows-dxgi";
        case WindowsCaptureBackend::kWgc:
            return "windows-wgc";
    }
    return "windows-unknown";
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

/**
 * @brief The Windows-specific failure modes.
 *
 * These are *distinct* on purpose: a host that gets
 * `kConsentDenied` should ask the user to grant screen recording,
 * while a host that gets `kDeviceLost` should re-open the device.
 *
 * The values are stable within v1.x.
 */
enum class NIMRTC_API_STABLE WindowsCaptureError : std::uint32_t {
    /** No error. */
    kNone = 0,

    /** The user refused the capture consent prompt, or the platform
     *  denies programmatic capture entirely. */
    kConsentDenied = 0x5701,

    /** The capture device was lost (display changed, GPU reset, the
     *  duplication became invalid).  The device can be re-opened. */
    kDeviceLost = 0x5702,

    /** The requested device does not exist (no such display / window). */
    kDeviceNotFound = 0x5703,

    /** The D3D11 device could not be created or was removed. */
    kD3dDeviceFailed = 0x5704,

    /** The requested format / region is not supported by the device. */
    kUnsupportedFormat = 0x5705,

    /** The capture session failed for a reason the backend cannot
     *  classify any further. */
    kCaptureFailed = 0x5706,
};

/** Human-readable error name (for logs). */
NIMRTC_API_STABLE inline std::string_view windows_capture_error_name(
        WindowsCaptureError e) noexcept {
    switch (e) {
        case WindowsCaptureError::kNone:             return "none";
        case WindowsCaptureError::kConsentDenied:    return "consent-denied";
        case WindowsCaptureError::kDeviceLost:       return "device-lost";
        case WindowsCaptureError::kDeviceNotFound:   return "device-not-found";
        case WindowsCaptureError::kD3dDeviceFailed:  return "d3d-device-failed";
        case WindowsCaptureError::kUnsupportedFormat:return "unsupported-format";
        case WindowsCaptureError::kCaptureFailed:    return "capture-failed";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

/** Windows-specific capture options. */
struct NIMRTC_API_STABLE WindowsCaptureOptions {
    /** Which backend to use.  Defaults to DXGI Desktop Duplication
     *  (the display path; the pointer is *not* composited). */
    WindowsCaptureBackend backend = WindowsCaptureBackend::kDxgiDesktopDuplication;

    /** Whether the WGC backend must verify the capture consent before
     *  opening.  When true and the consent is denied, `open()` fails
     *  with `WindowsCaptureError::kConsentDenied` instead of failing
     *  later in a less obvious way.
     *
     *  Ignored by the DXGI backend (DXGI needs no consent). */
    bool require_consent = true;

    /** Whether the backend may deliver frames as GPU surfaces.
     *
     *  When true (the default) and a GPU consumer is attached, the
     *  frame is delivered as a `GpuBuffer` without a host-memory
     *  round-trip.  When the GPU path is not usable the capture
     *  *falls back* to CPU staging and counts the copy in
     *  `CaptureStats::bytes_copied` — it never pretends to be
     *  zero-copy. */
    bool allow_zero_copy = true;

    /**
     * @brief The cursor source to composite into the frames.
     *
     * Null means "no cursor layer" — the frames are delivered exactly as
     * the backend produced them.  That is the right default for the WGC
     * backend, which already composites the pointer.
     *
     * For the DXGI backend the pointer is *not* in the frame, so a
     * source is what puts it there.  The source is **pluggable**: a
     * host may attach the platform source
     * (`make_platform_cursor_source()`) or its own.
     *
     * The device wraps whatever is attached in a `PinnedCursorFallback`,
     * so a cursor that becomes unavailable does not make the pointer
     * blink out of the picture — it is re-drawn at its last known
     * position instead.
     */
    std::shared_ptr<ICursorSource> cursor_source = nullptr;
};

// ---------------------------------------------------------------------------
// GPU frame delivery
// ---------------------------------------------------------------------------

/**
 * @brief One captured frame, delivered as a GPU surface.
 *
 * The frame owns a reference to the `GpuBuffer`; the receiver must
 * hold it alive for as long as it uses the texture (i.e. until the
 * encode call returns).  Releasing it afterwards is the receiver's
 * job.
 */
struct NIMRTC_API_STABLE GpuCaptureFrame {
    /** The captured surface.  Its handle is a `ID3D11Texture2D`
     *  (`GpuBackend::kWindowsD3D11Texture`), in `B8G8R8A8_UNORM`
     *  (BGRA) — the format the desktop compositor produces natively. */
    std::shared_ptr<video_frame::GpuBuffer> gpu_buffer;

    /** The changed region(s) of this frame, in source pixels.
     *
     *  The DXGI backend fills this from `GetFrameDirtyRects` /
     *  `GetFrameMoveRects`.  The WGC backend has no dirty-rect
     *  information, so it reports `full_frame = true` — the honest
     *  answer, not an empty rect that would mean "nothing changed". */
    plugins::CaptureDirtyRects dirty;

    /** Capture timestamp, microseconds (monotonic). */
    std::int64_t capture_ts_us = 0;

    /** Per-device frame sequence number (strictly increasing). */
    std::uint32_t frame_seq = 0;
};

/**
 * @brief Callback invoked with a GPU frame.
 *
 * Invoked on the capture device's own thread.  It must not block: the
 * implementation is expected to hand the frame straight to the encoder
 * and return.  The reference is valid for the duration of the call.
 */
using NIMRTC_API_STABLE GpuCaptureFrameCallback =
    std::function<void(GpuCaptureFrame&& frame)>;

/**
 * @brief Callback invoked when the capture device fails.
 *
 * The capture device stops after the callback returns; the host is
 * expected to re-open it (or to tear the session down).
 */
using NIMRTC_API_STABLE WindowsCaptureErrorCallback =
    std::function<void(WindowsCaptureError error, std::string_view message)>;

// ---------------------------------------------------------------------------
// WindowsCapture
// ---------------------------------------------------------------------------

/**
 * @brief The Windows capture device.
 *
 * Implements `plugins::IVideoCapture`.  The portable half — the
 * bounded queue, the drop policy, the dirty-rect model, the format
 * negotiation — comes from this module; only the leaf OS calls are
 * Windows-specific.
 */
class NIMRTC_API_STABLE WindowsCapture final : public plugins::IVideoCapture {
public:
    /** Construct a Windows capture device. */
    explicit WindowsCapture(plugins::CaptureConfig cfg,
                            WindowsCaptureOptions opts = {}) noexcept;

    ~WindowsCapture() override;

    WindowsCapture(const WindowsCapture&)            = delete;
    WindowsCapture& operator=(const WindowsCapture&) = delete;

    // ---- IPlugin ----------------------------------------------------------
    const char* name() const noexcept override;

    plugins::Status open() noexcept override;
    void close() noexcept override;

    // ---- IVideoCapture ----------------------------------------------------
    [[nodiscard]] std::vector<plugins::CaptureDevice>
    enumerate_devices() const noexcept override;

    [[nodiscard]] std::vector<plugins::CaptureFormat>
    supported_formats(const std::string& device_id) const noexcept override;

    [[nodiscard]] plugins::CaptureCapabilities
    capabilities() const noexcept override;

    [[nodiscard]] bool is_open() const noexcept override;

    [[nodiscard]] plugins::CaptureConfig config() const noexcept override;

    plugins::Status pop_frame(plugins::CaptureFrame& out) noexcept override;

    void set_frame_callback(plugins::CaptureFrameCallback cb) noexcept override;

    [[nodiscard]] plugins::CaptureStats stats() const noexcept override;

    // ---- Windows-specific -------------------------------------------------

    /**
     * Attach a GPU consumer.
     *
     * With a GPU callback attached, the capture device delivers the
     * surface *inside* the callback and does **not** push the frame
     * through the CPU copy queue — the point of the zero-copy path is
     * that the pixels never enter host memory.  Without one, the
     * device stages to the CPU and uses the queue, exactly like the
     * portable layer expects.
     */
    void set_gpu_frame_callback(GpuCaptureFrameCallback cb) noexcept;

    /** Attach a failure callback (consent refused, device lost, ...). */
    void set_error_callback(WindowsCaptureErrorCallback cb) noexcept;

    /**
     * @brief Attach (or detach) the cursor source.
     *
     * The cursor source is the *pluggable* half of plan A6: the device
     * composites whatever the source yields into the frames it delivers.
     * Passing null detaches the layer and the frames are delivered
     * exactly as the backend produced them.
     *
     * The source is wrapped in a `PinnedCursorFallback` internally, so
     * the pointer is re-drawn at its last known position whenever the
     * platform cannot be read — the "cursor is always drawn at the last
     * position" behaviour the plan asks for.
     *
     * Calling this while the device is open is allowed: the change
     * takes effect on the next frame.
     */
    void set_cursor_source(std::shared_ptr<ICursorSource> source) noexcept;

    /** The cursor source currently attached (may be null). */
    [[nodiscard]] std::shared_ptr<ICursorSource> cursor_source() const noexcept;

    /** The last error the device reported. */
    [[nodiscard]] WindowsCaptureError last_error() const noexcept;

    /** The backend this device uses. */
    [[nodiscard]] WindowsCaptureBackend backend() const noexcept {
        return opts_.backend;
    }

    /**
     * Re-open the device after a failure.
     *
     * The capture device stops itself when it hits an unrecoverable
     * error (consent refused, device lost).  A host that wants to keep
     * the session alive calls this; a host that does not simply tears
     * the session down.  Either way the device never crashes.
     */
    plugins::Status reopen() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    plugins::CaptureConfig cfg_;
    WindowsCaptureOptions opts_;
};

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------

/**
 * @brief Factory for the Windows capture devices.
 *
 * Registered under two ids, one per backend:
 *   - `"windows-dxgi"` — DXGI Desktop Duplication (displays).
 *   - `"windows-wgc"`  — Windows Graphics Capture (displays or windows).
 */
class NIMRTC_API_STABLE WindowsCaptureFactory final
    : public plugins::IVideoCaptureFactory {
public:
    explicit WindowsCaptureFactory(WindowsCaptureBackend backend) noexcept
        : backend_(backend) {}

    [[nodiscard]] std::string_view id() const noexcept override;

    [[nodiscard]] std::string_view display_name() const noexcept override;

    [[nodiscard]] plugins::IVideoCapture* create(
        plugins::CaptureConfig cfg) const override;

    /** The backend this factory produces. */
    [[nodiscard]] WindowsCaptureBackend backend() const noexcept {
        return backend_;
    }

private:
    WindowsCaptureBackend backend_;
};

/**
 * Register the Windows capture factories with `core::PluginRegistry`.
 *
 * Idempotent.  On a non-Windows build this is a no-op, so the
 * portable registration path stays identical on every platform.
 */
NIMRTC_API_STABLE void register_windows_capture_plugins() noexcept;

} // namespace nimrtc::video_capture
