/**
 * @file nimrtc/video_capture/capture_source_adapter.hpp
 * @brief The shared "capture device -> frame stream" adapter.
 *
 * The capture seam (`plugins::IVideoCapture`) describes a *device*: it
 * enumerates, negotiates a format and delivers frames into a bounded
 * queue.  The engine's video path, however, speaks `plugins::IVideoSource`
 * -- a plain stream of `VideoSourceFrame`s.
 *
 * This adapter is the bridge between the two, and it is deliberately
 * *shared*: every platform backend (Windows DXGI, macOS
 * ScreenCaptureKit, Linux PipeWire / X11) produces the same thing, so
 * they all feed the same adapter, and the engine never learns a platform
 * API (Layout Invariant 4).
 *
 * @code
 *   IVideoCapture  --device-->  CaptureSourceAdapter  --frames-->  IVideoSource
 *   (platform)                    (this file)                       (engine)
 * @endcode
 *
 * ## What the adapter does
 *
 *   1. Opens / closes the capture device in step with `start()` / `stop()`.
 *   2. Pops one frame from the capture's bounded queue per
 *      `produce_one()` call.
 *   3. Converts the capture's pixel format into the format the source
 *      was configured with (the encoders want I420; the desktop
 *      compositors deliver BGRA).
 *   4. Republishes the frame through the ordinary
 *      `VideoSourceFrameCallback`, carrying the capture timestamp and
 *      the frame sequence number.
 *
 * ## Format negotiation
 *
 * The capture device's `CaptureFormat::tag` is an opaque platform tag.
 * The adapter understands the tags the *portable* layer defines --
 * "bgra", "rgba", "nv12", "i420" -- and refuses anything else with
 * `kErrInvalidParam`, because guessing a pixel layout from an unknown
 * tag would produce garbage pixels rather than an error.  A platform
 * backend that delivers a different layout translates it in its own
 * adapter (that is what "a platform backend is a thin adapter over the
 * portable layer" means).
 *
 * ## The zero-copy caveat
 *
 * A capture frame may carry a GPU handle (`CaptureFrame::has_gpu_handle`)
 * instead of CPU pixels.  The adapter cannot expose a GPU handle through
 * `VideoSourceFrame` -- that struct is CPU-only -- so it passes such a
 * frame through *unchanged* only when the output format already matches
 * the capture's own format, and otherwise drops it (counting it as a
 * drop).  The hardware zero-copy path (capture surface -> encoder
 * without a CPU round-trip) is the platform backends' job (plan A3 /
 * A4 / A5); this adapter is the portable CPU path.
 *
 * @note Remote-desktop plan A2 / A2b.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/plugins/video_source.hpp>

namespace nimrtc::video_capture {

// ---------------------------------------------------------------------------
// Capture pixel formats
// ---------------------------------------------------------------------------

/**
 * @brief The pixel layouts the portable capture layer understands.
 *
 * A capture backend reports its layout through
 * `CaptureFormat::tag`; the adapter maps the tag onto this enum.
 */
enum class NIMRTC_API_STABLE CapturePixelFormat : std::uint8_t {
    kUnknown = 0,
    kBGRA,   ///< 32-bit BGRA -- what every desktop compositor produces
    kRGBA,   ///< 32-bit RGBA
    kNV12,   ///< YUV 4:2:0 semi-planar
    kI420,   ///< YUV 4:2:0 planar
};

/** Map a capture format tag onto a `CapturePixelFormat`.
 *
 *  @return `CapturePixelFormat::kUnknown` for a tag the portable layer
 *          does not know.  The adapter treats that as an error rather
 *          than guessing a layout.
 */
NIMRTC_API_STABLE CapturePixelFormat capture_pixel_format_from_tag(
    std::string_view tag) noexcept;

/** Human-readable pixel-format name (for logging). */
NIMRTC_API_STABLE std::string_view capture_pixel_format_name(
    CapturePixelFormat f) noexcept;

// ---------------------------------------------------------------------------
// CaptureSourceAdapter
// ---------------------------------------------------------------------------

/**
 * @brief Presents a capture device as an `IVideoSource`.
 *
 * The adapter does **not** own the capture device: the caller keeps it
 * alive for at least as long as the adapter is used.  This keeps the
 * ownership rule obvious -- a device is expensive and a host may want to
 * share one device between two adapters (a full-resolution stream and a
 * thumbnail stream, plan H3).
 *
 * Thread safety: the capture seam requires `start()` / `stop()` /
 * `produce_one()` to be serialized externally; the adapter inherits
 * that rule.
 */
class NIMRTC_API_STABLE CaptureSourceAdapter final
    : public plugins::IVideoSource {
public:
    /**
     * @param capture The capture device to drive.  Not owned.
     * @param cfg     The source configuration.  `cfg.format` is the
     *                *output* pixel format the frames are converted to;
     *                `cfg.width` / `cfg.height` are filled in from the
     *                device's negotiated format when they are zero.
     */
    CaptureSourceAdapter(plugins::IVideoCapture* capture,
                         plugins::VideoSourceConfig cfg) noexcept;

    ~CaptureSourceAdapter() override;

    CaptureSourceAdapter(const CaptureSourceAdapter&)            = delete;
    CaptureSourceAdapter& operator=(const CaptureSourceAdapter&) = delete;

    // ---- IPlugin ----------------------------------------------------------

    /** The adapter's plugin name. */
    const char* name() const noexcept override {
        return "nimrtc::video_capture::CaptureSourceAdapter";
    }

    /** Opens the capture device.
     *
     *  This is the `IPlugin` open phase; it is the same operation as
     *  `start()`, and the two are kept in step: `open()` is what the
     *  engine's plugin lifecycle calls, `start()` is what the source
     *  seam calls.
     */
    plugins::Status open() noexcept override { return start(); }

    /** Closes the capture device.  Idempotent. */
    void close() noexcept override { stop(); }

    // ---- IVideoSource ------------------------------------------------------

    /** Opens the capture device.  @return kErrInvalidParam when the
     *  device negotiates a format the adapter cannot interpret. */
    plugins::Status start() noexcept override;

    /** Closes the capture device.  Idempotent. */
    void stop() noexcept override;

    [[nodiscard]] bool running() const noexcept override {
        return running_;
    }

    /** Pops one frame from the capture queue and republishes it.
     *
     *  The callback fires synchronously, before this method returns.
     *  When the capture queue is empty the call is a no-op -- the
     *  engine's tick loop must not be parked by a quiet device.
     */
    void produce_one() noexcept override;

    /** The source configuration, with the negotiated dimensions folded in. */
    [[nodiscard]] plugins::VideoSourceConfig config() const noexcept override {
        return cfg_;
    }

    /** A capture device's content is its own; this is a no-op. */
    void set_pattern(plugins::VideoSourcePattern p) noexcept override {
        (void)p;
    }

    void set_callback(plugins::VideoSourceFrameCallback cb) noexcept override {
        cb_ = std::move(cb);
    }

    [[nodiscard]] Stats stats() const noexcept override;

    /** True iff the capture device reports a zero-copy path. */
    [[nodiscard]] bool is_hardware_accelerated() const noexcept override;

    /** The capture backend's identifier, e.g. "synthetic", "dxgi-desktop-dup". */
    [[nodiscard]] std::string_view hardware_backend() const noexcept override;

    // ---- Capture-specific --------------------------------------------------

    /** The capture device this adapter drives. */
    [[nodiscard]] plugins::IVideoCapture* capture() const noexcept {
        return capture_;
    }

    /** The changed region(s) reported with the last delivered frame.
     *
     *  This is the dirty-rect metadata the encoder needs to skip the
     *  unchanged part of a static desktop (plan A7).  It is valid until
     *  the next `produce_one()`.
     */
    [[nodiscard]] const plugins::CaptureDirtyRects& last_dirty() const noexcept {
        return last_dirty_;
    }

private:
    /** Convert the popped frame into the output format.  @return false when
     *  the frame cannot be represented in the output format (in which case
     *  it is counted as a drop). */
    bool convert(const plugins::CaptureFrame& in) noexcept;

    plugins::IVideoCapture* capture_ = nullptr;
    plugins::VideoSourceConfig cfg_{};
    plugins::VideoSourceFrameCallback cb_;

    /** The pixel format the device negotiated. */
    CapturePixelFormat device_format_ = CapturePixelFormat::kUnknown;

    /** The capture backend's identifier, cached at `start()`.  A
     *  `string_view` into `capabilities().backend` would dangle, because
     *  `capabilities()` returns by value. */
    std::string backend_;

    /** Scratch buffer for the converted frame. */
    std::vector<std::uint8_t> scratch_;

    /** The frame handed to the callback.  Its plane pointers point into
     *  `scratch_` (or into the capture's own buffer when no conversion
     *  was needed). */
    plugins::VideoSourceFrame out_{};

    plugins::CaptureDirtyRects last_dirty_{};

    std::uint64_t produced_ = 0;
    std::uint64_t dropped_  = 0;
    bool running_ = false;
};

} // namespace nimrtc::video_capture
