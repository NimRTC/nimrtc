/**
 * @file nimrtc/video_capture/unix_capture_core.hpp
 * @brief The portable capture core shared by the macOS and Linux backends.
 *
 * This is the *mechanism* half of remote-desktop plan items **A4** (macOS
 * ScreenCaptureKit) and **A5** (Linux XDG Desktop Portal + PipeWire, with
 * an X11 fallback).  The plan asks for exactly this shape:
 *
 * > "Only the leaf OS call differs per platform."  (A2b)
 *
 * So the *leaf* differences — "give me the next frame", "list the
 * devices", "ask the user for permission" — live in the platform
 * adapters under `src/platform/macos/` and `src/platform/linux/`, and
 * everything else lives here: the bounded queue, the frame-sequence
 * counter, the statistics, the format negotiation, the region crop and
 * the dirty-rectangle bookkeeping.
 *
 * ## Why this file is portable
 *
 * Nothing in here touches an OS API.  It is pure C++20 over raw bytes,
 * which means it can be — and is — **unit-tested on every CI leg**,
 * including the Windows leg where the macOS and Linux backends cannot
 * even be compiled.  That is the whole point of splitting it out: the
 * parts that are hard to get right (the drop policy, the diff, the
 * crop, the negotiation) are verified everywhere, and the parts that
 * are hard to *compile* (the ScreenCaptureKit calls, the D-Bus portal
 * handshake) are thin enough to read.
 *
 * ## The dirty-rectangle model (plan A7)
 *
 * Windows DXGI hands out changed rectangles natively.  macOS and Linux
 * do not, so the plan calls for "a conservative bounding box from a
 * pixel-diff, or full-frame when unavailable":
 *
 *   - `compute_pixel_dirty_rects()` is the portable pixel-diff.  It
 *     compares two frames of raw bytes, finds the changed *runs* per
 *     row, and merges the runs that are close together into
 *     rectangles.  It is a pure function over bytes, so it is tested
 *     directly.
 *   - When the diff is disabled — or when there is no previous frame to
 *     compare against — the honest answer is `full_frame = true`.
 *     An empty rectangle list would mean "nothing changed", which is a
 *     lie the encoder would act on.
 *
 * The diff is deliberately *conservative*: it may report a rectangle
 * that is larger than the changed area (runs that are close together
 * get merged), but it never reports a smaller one.  A too-large
 * rectangle costs a few wasted bytes; a too-small one costs a
 * corrupted picture.
 *
 * ## The region crop (plan A8)
 *
 * `crop_frame_bgra()` crops a full frame down to a `CaptureRegion` in
 * *source* pixels.  It is a pure function as well, so the "an 800x600
 * region produces frames of exactly that size" property is asserted
 * without a display attached.
 *
 * ## Format negotiation
 *
 * `negotiate_capture_format()` implements the frozen semantics of
 * `plugins::CaptureFormat`: never upscale, fill in the opaque `tag`,
 * and report `zero_copy` / `has_dirty_rects` *honestly* — that is, per
 * what the backend can actually deliver, not per what the caller asked
 * for.  A request the device cannot honour is clamped, and the caller
 * reads the result back from `config()`.
 *
 * @note Remote-desktop plan A4 / A5 / A7 / A8, building on A2b.
 */

#ifndef NIMRTC_VIDEO_CAPTURE_UNIX_CAPTURE_CORE_HPP
#define NIMRTC_VIDEO_CAPTURE_UNIX_CAPTURE_CORE_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <nimrtc/core/api_version.hpp>
#include <nimrtc/plugins/video_capture.hpp>
#include <nimrtc/video_capture/capture_queue.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace nimrtc::video_capture {

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

/**
 * @brief The Unix capture backends' failure modes.
 *
 * These are *distinct* on purpose, and they mirror
 * `WindowsCaptureError` (plan A3): a host that gets
 * `kPermissionDenied` should ask the user to grant screen recording,
 * while a host that gets `kDeviceLost` should re-open the device.  The
 * two call for completely different responses, so they must not be
 * collapsed into one "capture failed".
 *
 * The values are stable within v1.x.
 */
enum class NIMRTC_API_STABLE UnixCaptureError : std::uint32_t {
    /** No error. */
    kNone = 0,

    /** The user refused the capture permission prompt, or the platform
     *  denies programmatic capture entirely (macOS screen-recording
     *  consent, a Wayland portal request the user dismissed). */
    kPermissionDenied = 0x5801,

    /** The requested device does not exist (no such display / window /
     *  portal source). */
    kDeviceNotFound = 0x5802,

    /** The requested format / region is not supported by the device. */
    kUnsupportedFormat = 0x5803,

    /** The capture session failed for a reason the backend cannot
     *  classify any further. */
    kCaptureFailed = 0x5804,

    /** The capture device was lost (the display changed, the portal
     *  session ended, the PipeWire stream died).  The device can be
     *  re-opened. */
    kDeviceLost = 0x5805,

    /** No display server is reachable at all: neither `WAYLAND_DISPLAY`
     *  nor `DISPLAY` is set, or the connection to the one that is set
     *  failed. */
    kDisplayUnavailable = 0x5806,

    /** The XDG Desktop Portal is not reachable (Wayland session without
     *  a portal service), so the platform cannot ask the user for a
     *  source. */
    kPortalUnavailable = 0x5807,
};

/** Human-readable error name (for logs). */
NIMRTC_API_STABLE inline std::string_view unix_capture_error_name(UnixCaptureError e) noexcept {
    switch (e) {
    case UnixCaptureError::kNone:
        return "none";
    case UnixCaptureError::kPermissionDenied:
        return "permission-denied";
    case UnixCaptureError::kDeviceNotFound:
        return "device-not-found";
    case UnixCaptureError::kUnsupportedFormat:
        return "unsupported-format";
    case UnixCaptureError::kCaptureFailed:
        return "capture-failed";
    case UnixCaptureError::kDeviceLost:
        return "device-lost";
    case UnixCaptureError::kDisplayUnavailable:
        return "display-unavailable";
    case UnixCaptureError::kPortalUnavailable:
        return "portal-unavailable";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

/**
 * @brief Options shared by the Unix capture backends.
 *
 * These are the knobs the portable core understands.  A platform
 * adapter may add its own on top (the macOS backend adds the
 * permission policy; the PipeWire backend adds the portal interaction),
 * but it must express the *capture* behaviour through these.
 */
struct NIMRTC_API_STABLE UnixCaptureOptions {
    /** Whether the backend may deliver frames as GPU surfaces.
     *
     *  When true and the backend has a native surface (macOS's
     *  `CVPixelBuffer`), the frame is delivered as a `GpuBuffer` with
     *  `GpuBackend::kAppleCFPixelBuffer` and the pixels never enter
     *  host memory.  When the GPU path is not usable the capture falls
     *  back to CPU staging and counts the copy in
     *  `CaptureStats::bytes_copied` — it never pretends to be
     *  zero-copy. */
    bool allow_zero_copy = true;

    /** Whether the portable pixel-diff supplies the dirty rectangles.
     *
     *  True by default: it is the A7 fallback for the platforms that
     *  do not report changed regions natively (which is all of
     *  macOS and Linux today).  When false, every frame is reported as
     *  `full_frame`. */
    bool enable_pixel_diff = true;

    /** How close two changed runs must be (in pixels) to be merged
     *  into one rectangle.
     *
     *  A larger value produces fewer, larger rectangles (cheaper for
     *  the encoder, slightly more wasted bytes).  Zero merges only
     *  directly-adjacent runs. */
    std::uint32_t diff_merge_threshold = 64;

    /** The frame rate used when the caller asks for "the device's
     *  best" and the device does not state one. */
    std::uint32_t default_fps = 30;

    /** Whether the backend must verify the capture permission before
     *  opening.
     *
     *  When true and the permission is denied, `open()` fails with
     *  `UnixCaptureError::kPermissionDenied` instead of failing later
     *  in a less obvious way (an endless stream of black frames, on
     *  macOS). */
    bool require_permission = true;
};

/** Options for the portable pixel-diff. */
struct NIMRTC_API_STABLE UnixCaptureDiffOptions {
    /** Whether the diff runs at all.  When false, the result is
     *  always `full_frame`. */
    bool enabled = true;

    /** How close two changed runs must be (in pixels) to be merged. */
    std::uint32_t merge_threshold = 64;

    /** The number of bytes per pixel (4 for BGRA, which is what every
     *  desktop compositor produces). */
    std::uint32_t bytes_per_pixel = 4;

    /** The row stride of the *current* frame, in bytes.  0 means the
     *  frame is packed (`width * bytes_per_pixel`). */
    std::int32_t stride = 0;

    /** The row stride of the *previous* frame, in bytes.  0 means the
     *  frame is packed. */
    std::int32_t previous_stride = 0;
};

// ---------------------------------------------------------------------------
// The portable pixel-diff (plan A7)
// ---------------------------------------------------------------------------

/**
 * @brief Compute the changed region(s) between two frames.
 *
 * A pure function over raw bytes: it compares the two frames, finds
 * the changed *runs* per row, and merges the runs that are close
 * together into rectangles.  Because it is pure, it is unit-tested
 * directly, on any platform, with no display attached.
 *
 * @param previous  The previous frame's pixels.  Null means "there is
 *                  nothing to compare against" — the result is then
 *                  `full_frame`, which is the honest answer.
 * @param current   The current frame's pixels.  Null is treated as
 *                  "everything changed".
 * @param width     The frame width in pixels.
 * @param height    The frame height in pixels.
 * @param options   The diff options.
 * @return The changed region(s).  `full_frame == false` with an empty
 *         rectangle list means "nothing changed", which is the
 *         property that makes a static desktop nearly free to send.
 */
NIMRTC_API_STABLE plugins::CaptureDirtyRects
compute_pixel_dirty_rects(const std::uint8_t* previous, const std::uint8_t* current,
                          std::uint32_t width, std::uint32_t height,
                          const UnixCaptureDiffOptions& options) noexcept;

/**
 * @brief Clamp a region to the source surface.
 *
 * A region that reaches outside the surface is truncated (never
 * wrapped, never negative).  A zero-sized region means "the whole
 * surface" and is returned unchanged.
 */
NIMRTC_API_STABLE plugins::CaptureRegion clamp_region(const plugins::CaptureRegion& region,
                                                      std::uint32_t src_width,
                                                      std::uint32_t src_height) noexcept;

/**
 * @brief Crop a BGRA frame down to a region.
 *
 * A pure function: the cropped pixels are written into @p out, and the
 * cropped geometry is reported through the output parameters.  The
 * alpha byte is forced to 0xFF, because the source pictures come from
 * a compositor that leaves the fourth byte undefined.
 *
 * @param src        The source pixels.
 * @param src_size   The size of @p src in bytes.
 * @param src_width  The source width in pixels.
 * @param src_height The source height in pixels.
 * @param src_stride The source row stride in bytes.
 * @param region     The region to crop (in source pixels).
 * @param out        Receives the cropped pixels.  Reallocated as
 *                   needed.
 * @param out_width  Receives the cropped width.
 * @param out_height Receives the cropped height.
 * @param out_stride Receives the cropped row stride in bytes.
 * @return true on success.
 */
NIMRTC_API_STABLE bool crop_frame_bgra(const std::uint8_t* src, std::size_t src_size,
                                       std::uint32_t src_width, std::uint32_t src_height,
                                       std::int32_t src_stride,
                                       const plugins::CaptureRegion& region,
                                       std::vector<std::uint8_t>& out, std::uint32_t& out_width,
                                       std::uint32_t& out_height,
                                       std::int32_t& out_stride) noexcept;

/**
 * @brief Negotiate the format a device will actually deliver.
 *
 * The frozen semantics of `plugins::CaptureFormat` apply:
 *
 *   - The result is **never larger than the device** (no upscaling),
 *     and a request larger than the device is *clamped down* rather
 *     than refused.
 *   - The opaque `tag` is filled in when the caller left it empty.
 *   - `zero_copy` and `has_dirty_rects` report what the backend can
 *     honestly deliver, not what the caller hoped for.
 *
 * @param device             The device the format is negotiated for.
 * @param requested          The requested format.  A zero width/height
 *                           means "the device's best".
 * @param default_tag        The tag used when the request carries none.
 * @param zero_copy_capable  Whether the backend can deliver the
 *                           negotiated format without a CPU readback.
 * @param dirty_rects_capable Whether the backend reports per-frame
 *                           dirty rectangles.
 */
NIMRTC_API_STABLE plugins::CaptureFormat
negotiate_capture_format(const plugins::CaptureDevice& device,
                         const plugins::CaptureFormat& requested, const std::string& default_tag,
                         bool zero_copy_capable, bool dirty_rects_capable) noexcept;

// ---------------------------------------------------------------------------
// UnixCaptureCore
// ---------------------------------------------------------------------------

/**
 * @brief The portable core of the Unix capture backends.
 *
 * It owns the bounded queue, the frame-sequence counter, the
 * statistics, the negotiated format, the region handling and the
 * dirty-rect bookkeeping.  A platform adapter drives it: it calls
 * `submit_frame()` (or `submit_gpu_frame()`) with what the OS handed
 * it, and the core does the rest.
 *
 * Thread safety: `submit_frame()` is called from the capture thread
 * and `pop_frame()` from the engine's tick thread; the two are
 * serialized internally.  The accessors are safe to call from either.
 */
class NIMRTC_API_STABLE UnixCaptureCore {
  public:
    /** Construct a core with the given configuration and options. */
    explicit UnixCaptureCore(plugins::CaptureConfig cfg = {},
                             UnixCaptureOptions opts = {}) noexcept;

    ~UnixCaptureCore();

    UnixCaptureCore(const UnixCaptureCore&) = delete;
    UnixCaptureCore& operator=(const UnixCaptureCore&) = delete;

    // ---- Configuration ----------------------------------------------------

    /** Replace the requested configuration (before `open()`). */
    void set_config(const plugins::CaptureConfig& cfg) noexcept;

    /** The configuration in force, with the negotiated format
     *  substituted for the requested one. */
    [[nodiscard]] plugins::CaptureConfig config() const noexcept;

    /** The options in force. */
    [[nodiscard]] UnixCaptureOptions options() const noexcept;

    /** Replace the options (before `open()`). */
    void set_options(const UnixCaptureOptions& opts) noexcept;

    /** The region the capture is cropped to. */
    [[nodiscard]] plugins::CaptureRegion region() const noexcept;

    /** Replace the region the capture is cropped to. */
    void set_region(const plugins::CaptureRegion& region) noexcept;

    /** The capabilities the backend reported. */
    [[nodiscard]] plugins::CaptureCapabilities capabilities() const noexcept;

    /** Record the capabilities the backend reported. */
    void set_capabilities(const plugins::CaptureCapabilities& caps) noexcept;

    /** Record the format that was actually negotiated. */
    void set_negotiated_format(const plugins::CaptureFormat& format) noexcept;

    /** The format that was actually negotiated. */
    [[nodiscard]] plugins::CaptureFormat negotiated_format() const noexcept;

    /**
     * @brief Negotiate the format for a device.
     *
     * A convenience over the free `negotiate_capture_format()` that
     * takes the core's own capabilities and options into account.
     */
    [[nodiscard]] plugins::CaptureFormat
    negotiate_format(const plugins::CaptureDevice& device,
                     const plugins::CaptureFormat& requested) const noexcept;

    // ---- Submission (the capture thread) ----------------------------------

    /**
     * @brief Submit a CPU frame.
     *
     * The frame is cropped to the configured region, the dirty
     * rectangles are computed (unless the caller supplied them), and
     * the result is pushed into the bounded queue.
     *
     * @param pixels  The full-frame pixels.  May be null for a
     *                metadata-only frame.
     * @param size    The size of @p pixels in bytes.
     * @param stride  The row stride in bytes.
     * @param width   The full-frame width in pixels.
     * @param height  The full-frame height in pixels.
     * @param ts      The capture timestamp (microseconds).
     * @param dirty   The changed region(s), or null to let the core
     *                compute them with the portable pixel-diff.
     */
    plugins::Status submit_frame(const std::uint8_t* pixels, std::size_t size, std::int32_t stride,
                                 std::uint32_t width, std::uint32_t height, std::int64_t ts,
                                 const plugins::CaptureDirtyRects* dirty = nullptr) noexcept;

    /**
     * @brief Submit a GPU frame (the zero-copy path).
     *
     * The surface is handed over *as is* — the core never reads it
     * back — so the pixels never enter host memory.  The handle is
     * **borrowed**: it must stay valid until the next `pop_frame()`,
     * exactly like the `pixels` pointer.
     *
     * @param pixels  An optional CPU mirror of the surface.  Null on a
     *                pure zero-copy frame.
     * @param backend The GPU backend that owns @p handle.
     * @param handle  The native GPU resource.
     * @param pitch   The native row pitch in bytes (0 when the backend
     *                has an implicit layout).
     * @param owner   An optional owner of @p handle.  The core keeps it
     *                alive until the frame is popped, so the adapter
     *                does not have to manage the handle's lifetime by
     *                hand.  It must point at @p handle.
     */
    plugins::Status submit_gpu_frame(const std::uint8_t* pixels, std::size_t size,
                                     std::int32_t stride, std::uint32_t width, std::uint32_t height,
                                     std::int64_t ts, plugins::GpuBackend backend, void* handle,
                                     std::uint32_t pitch,
                                     const plugins::CaptureDirtyRects* dirty = nullptr,
                                     std::shared_ptr<void> owner = nullptr) noexcept;

    // ---- Consumption (the engine tick thread) ----------------------------

    /**
     * @brief Pop the next frame.
     *
     * @param out Receives the frame.  The pixels stay valid until the
     *            next `pop_frame()`.
     * @return `kOk` when a frame was delivered, `kErrNotReady` when the
     *         queue is empty.
     */
    plugins::Status pop_frame(plugins::CaptureFrame& out) noexcept;

    // ---- Statistics -------------------------------------------------------

    /** The capture counters. */
    [[nodiscard]] plugins::CaptureStats stats() const noexcept;

    /** Zero the counters. */
    void reset_stats() noexcept;

    /** Count a device failure (a frame the backend could not
     *  deliver). */
    void note_device_error() noexcept;

    /** Count the bytes copied between the device and host memory (the
     *  CPU-staging path). */
    void add_bytes_copied(std::uint64_t bytes) noexcept;

    /** Count a device re-open. */
    void note_device_restart() noexcept;

    /** Count a mid-stream format change. */
    void note_format_change() noexcept;

    // ---- Delivery callback ------------------------------------------------

    /** Install the frame-delivery callback (the capture thread's). */
    void set_frame_callback(plugins::CaptureFrameCallback cb) noexcept;

    // ---- Queue policy -----------------------------------------------------

    /** Set the queue's overflow policy. */
    void set_drop_policy(plugins::CaptureDropPolicy policy) noexcept;

    /** Set the queue's depth. */
    void set_queue_depth(std::uint32_t depth) noexcept;

    /** Whether the portable pixel-diff is enabled. */
    [[nodiscard]] bool pixel_diff_enabled() const noexcept;

    /** Enable or disable the portable pixel-diff. */
    void set_pixel_diff_enabled(bool enabled) noexcept;

    /** The merge threshold the pixel-diff uses. */
    [[nodiscard]] std::uint32_t merge_threshold() const noexcept;

    /** Set the merge threshold the pixel-diff uses. */
    void set_merge_threshold(std::uint32_t threshold) noexcept;

  private:
    plugins::Status push_common(const std::uint8_t* pixels, std::size_t size, std::int32_t stride,
                                std::uint32_t width, std::uint32_t height, std::int64_t ts,
                                plugins::GpuBackend backend, void* handle, std::uint32_t pitch,
                                const plugins::CaptureDirtyRects* dirty,
                                std::shared_ptr<void> owner) noexcept;

    void fire_callback() noexcept;

    /** One zero-copy submission, waiting to be handed to the consumer. */
    struct GpuSubmission {
        std::uint32_t seq = 0;
        plugins::GpuBackend backend = plugins::GpuBackend::kNone;
        void* handle = nullptr;
        std::uint32_t pitch = 0;
        std::shared_ptr<void> owner;
    };

    mutable std::mutex mutex_;

    plugins::CaptureConfig cfg_{};
    UnixCaptureOptions opts_{};
    plugins::CaptureCapabilities caps_{};
    plugins::CaptureFormat negotiated_{};

    CaptureQueue queue_{2};
    plugins::CaptureFrameCallback cb_;

    plugins::CaptureStats stats_{};

    /** The previous frame's pixels, kept for the pixel-diff. */
    std::vector<std::uint8_t> prev_pixels_;
    std::uint32_t prev_width_ = 0;
    std::uint32_t prev_height_ = 0;
    std::int32_t prev_stride_ = 0;
    bool have_prev_ = false;

    /** Scratch buffer for the cropped frame. */
    std::vector<std::uint8_t> cropped_;

    /** The zero-copy submissions that have not been popped yet, keyed
     *  by frame sequence number.  The queue copies pixels, so the GPU
     *  handle travels beside it. */
    std::map<std::uint32_t, GpuSubmission> gpu_pending_;

    /** The most recently handed-out GPU submission.  It keeps the
     *  handle alive until the *next* `pop_frame()`, which is exactly
     *  what the `CaptureFrame` contract promises. */
    GpuSubmission last_gpu_;

    std::uint32_t next_seq_ = 0;
};

} // namespace nimrtc::video_capture
#endif // NIMRTC_VIDEO_CAPTURE_UNIX_CAPTURE_CORE_HPP
