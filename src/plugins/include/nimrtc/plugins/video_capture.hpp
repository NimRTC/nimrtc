/**
 * @file nimrtc/plugins/video_capture.hpp
 * @brief IVideoCapture — pluggable *device* capture interface.
 *
 * The existing `IVideoSource` seam (see `nimrtc/plugins/video_source.hpp`)
 * describes a *stream of raw frames* — the memory test-pattern source is
 * the one implementation that ships.  That seam is deliberately dumb: it
 * has no notion of a device, no enumeration, no region selection and no
 * format negotiation.  A remote-desktop product needs all four, plus
 * two things the source seam has no room for:
 *
 *   - **Dirty-rect / incremental metadata** (remote desktop's whole point:
 *     a static screen must not cost a full frame every tick), and
 *   - **A capability query** (does this device offer zero-copy? does it
 *     composite the cursor? what is its maximum resolution?), so the
 *     engine can pick a backend and a ladder rung *before* the first
 *     frame arrives.
 *
 * So this is a *different* seam with a *different* shape, layered on the
 * source seam rather than replacing it:
 *
 * @code
 *   IVideoCapture  ──device enumeration, negotiation, region, capabilities
 *        │  (asynchronous, bounded queue, explicit drop policy)
 *        ▼
 *   IVideoSource  ──the frame stream the engine already knows how to pump
 *        │
 *        ▼
 *   IVideoCodec   ──encoder (the hardware path, zero-copy where possible)
 * @endcode
 *
 * A capture plug-in implements `IVideoCapture`; the *engine* (or a thin
 * adapter in `src/modules/video_capture/`) turns it into an
 * `IVideoSource` the existing pipeline can consume.  The engine therefore
 * never learns a platform API.
 *
 * ## Why asynchronous
 *
 * Device capture is *push*: the OS tells you a new frame exists (a
 * DXGI duplication event, a ScreenCaptureKit stream, a PipeWire buffer)
 * on a thread the plugin owns (RD-6: a capture plug-in may own its own
 * thread; the engine stays a single-threaded orchestrator).  The engine
 * then *pulls* from the capture's bounded queue on its tick thread and
 * hands the frame to the encoder.
 *
 * The queue is bounded on purpose.  If the encoder cannot keep up, the
 * right behaviour for a remote desktop is to drop a frame, not to grow a
 * backlog and add latency without bound.  The drop policy is explicit
 * and configurable (see `CaptureDropPolicy`): for screen content, dropping
 * the *oldest* frame is correct (the newest frame is the truth); for a
 * camera, dropping the *newest* is often preferable (lower latency).
 *
 * ## What is NOT here
 *
 * No pixel formats, no `VideoSourceFrame`, no OS headers.  This header
 * is pure C++20 and knows nothing about how the pixels are represented —
 * the *device* describes its own output through `CaptureFormat`, and the
 * adapter translates.
 *
 * @note Remote-desktop plan A1 (M1-b). Interface is stable.
 */

// base.hpp must be before include guard — see transport.hpp for rationale.
#include <nimrtc/core/api_version.hpp>
#include "nimrtc/plugins/base.hpp"
// `IVideoSource` / `VideoSourceConfig` are the *source* seam the capture
// device is presented through.  A capture plug-in does not have to know
// about them -- but the *factory* does, because the factory is what
// hands the engine a source it can drive (see
// `IVideoCaptureFactory::create_source`).
#include "nimrtc/plugins/video_source.hpp"
// `GpuHandle` is the zero-copy currency (plan B10).  It lives in the
// video_frame module, which `nimrtc::plugins` already re-exports
// (see video_codec.hpp) — but video_capture.hpp must not depend on
// video_codec.hpp's *contents*, only on the shared type.
#include <nimrtc/video_frame/frame.hpp>

#ifndef NIMRTC_PLUGINS_VIDEO_CAPTURE_HPP
#define NIMRTC_PLUGINS_VIDEO_CAPTURE_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace nimrtc::plugins {

// ---------------------------------------------------------------------------
// Shared type aliases
// ---------------------------------------------------------------------------

// `GpuHandle` is the zero-copy currency (remote-desktop plan B10): it
// names a GPU surface and the device it lives on.  It is defined in the
// video_frame module (the single source of truth) and re-exported here
// so a capture plug-in can name it without pulling in the codec seam.
using NIMRTC_API_STABLE GpuHandle  = video_frame::GpuHandle;
using NIMRTC_API_STABLE GpuBackend = video_frame::GpuBackend;

// ---------------------------------------------------------------------------
// Device identity
// ---------------------------------------------------------------------------

/** What kind of thing is being captured. */
enum class NIMRTC_API_STABLE CaptureDeviceKind : std::uint8_t {
    kUnknown   = 0,
    kScreen    = 1,   ///< a display / monitor (or the whole virtual desktop)
    kWindow    = 2,   ///< an application window
    kCamera    = 3,   ///< a physical camera
};

/** Human-readable device-kind name (for logging / UI). */
inline NIMRTC_API_STABLE std::string_view capture_device_kind_name(
        CaptureDeviceKind k) noexcept {
    switch (k) {
        case CaptureDeviceKind::kScreen: return "screen";
        case CaptureDeviceKind::kWindow: return "window";
        case CaptureDeviceKind::kCamera: return "camera";
        default:                        return "unknown";
    }
}

/** One capturable device. */
struct NIMRTC_API_STABLE CaptureDevice {
    /** Stable id for this device within the capture backend, e.g.
     *  `"\\\\.\\DISPLAY1"` on Windows, `"0"` for a Linux monitor index.
     *  Opaque to the engine — it is passed back verbatim to `open()`. */
    std::string id;

    /** Human-readable name, e.g. "Display 1" or "Webcam 0". */
    std::string name;

    CaptureDeviceKind kind = CaptureDeviceKind::kUnknown;

    /** Maximum width the device can deliver at its highest frame rate. */
    std::uint32_t max_width  = 0;
    /** Maximum height the device can deliver at its highest frame rate. */
    std::uint32_t max_height = 0;

    /** Maximum frame rate the device can deliver at full resolution. */
    std::uint32_t max_fps = 0;
};

// ---------------------------------------------------------------------------
// Format negotiation
// ---------------------------------------------------------------------------

/**
 * @brief A format the device *can* deliver.
 *
 * The engine asks the device what it supports, then picks one.  The
 * choice is not free-form: a remote desktop wants the largest format the
 * device offers *without* a CPU round-trip (zero-copy), and a camera
 * wants the format that matches the encoder.
 */
struct NIMRTC_API_STABLE CaptureFormat {
    /** Width in pixels. */
    std::uint32_t width  = 0;
    /** Height in pixels. */
    std::uint32_t height = 0;

    /** Frame rate. */
    std::uint32_t fps = 0;

    /** Opaque platform format tag. The engine does not interpret it; it
     *  is compared for equality against the device's own list and handed
     *  back to the device in `open()`.  An empty tag means "the device's
     *  default format at this size". */
    std::string tag;

    /** True iff this format can be delivered without a CPU readback of
     *  the captured surface. */
    bool zero_copy = false;

    /** True iff this format carries per-frame dirty-rectangle metadata. */
    bool has_dirty_rects = false;
};

// ---------------------------------------------------------------------------
// Region selection
// ---------------------------------------------------------------------------

/** A sub-region of the captured surface, in *source* pixels. */
struct NIMRTC_API_STABLE CaptureRegion {
    std::uint32_t x      = 0;
    std::uint32_t y      = 0;
    std::uint32_t width  = 0;
    std::uint32_t height = 0;

    /** True iff the region covers the whole source surface. */
    [[nodiscard]] bool is_full_frame(std::uint32_t src_w,
                                     std::uint32_t src_h) const noexcept {
        return x == 0 && y == 0 &&
               width == src_w && height == src_h;
    }
};

// ---------------------------------------------------------------------------
// Dirty-rectangle metadata
// ---------------------------------------------------------------------------

/**
 * @brief The changed region(s) of a captured frame.
 *
 * `full_frame` means "everything changed, ignore the rects" — the
 * conservative answer a device gives when it has no per-pixel diff
 * information (which is the common case outside Windows DXGI).
 */
struct NIMRTC_API_STABLE CaptureDirtyRects {
    /** True iff the whole frame must be treated as changed. */
    bool full_frame = true;

    /** The changed rectangles, in source pixels. Empty when `full_frame`. */
    std::vector<CaptureRegion> rects;
};

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------

/**
 * @brief What a capture backend can do.
 *
 * Queried *before* the first frame, so the engine can pick a ladder rung
 * and decide whether the zero-copy path is available on this machine.
 */
struct NIMRTC_API_STABLE CaptureCapabilities {
    /** The device can deliver frames without a CPU readback. */
    bool zero_copy = false;

    /** The device reports per-frame dirty rectangles. */
    bool dirty_rects = false;

    /** The device composites the pointer into the frame itself (so the
     *  engine does not need a separate cursor layer). */
    bool cursor_composited = false;

    /** The device can capture a sub-region rather than a whole surface. */
    bool region_selection = false;

    /** The device can enumerate itself (the list is not hard-coded). */
    bool enumeration = false;

    /** Largest frame the device can deliver, in pixels (width * height). */
    std::uint64_t max_pixels = 0;

    /** Backend identifier, e.g. "dxgi-desktop-dup", "screen-capture-kit",
     *  "x11", "pipewire", "synthetic". */
    std::string backend;
};

// ---------------------------------------------------------------------------
// Drop policy
// ---------------------------------------------------------------------------

/**
 * @brief What to drop when the bounded queue overflows.
 *
 * The queue exists to bound latency, not to buffer.  When the consumer
 * cannot keep up, one frame has to go.
 */
enum class NIMRTC_API_STABLE CaptureDropPolicy : std::uint8_t {
    /** Drop the oldest frame and keep the newest (correct for screen
     *  content: the newest frame is the truth, and a stale frame is
     *  worth nothing). This is the default. */
    kDropOldest = 0,

    /** Drop the newest frame and keep the one already queued (correct for
     *  a camera: a slightly stale frame beats a frame that never arrives
     *  in time to be displayed). */
    kDropNewest = 1,

    /** Drop the frame that arrives when the queue is full, and do not
     *  replace anything (a strict "no reordering" policy). */
    kDropIncoming = 2,
};

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/** How a capture instance is configured. */
struct NIMRTC_API_STABLE CaptureConfig {
    /** The device to open. Left empty, the backend picks its default
     *  (for a screen backend: the primary display). */
    std::string device_id;

    /** The format to negotiate. Zero width/height means "the device's
     *  best". */
    CaptureFormat format;

    /** The region to capture. A zero-sized region means the whole
     *  surface. */
    CaptureRegion region;

    /** Maximum frames held in the bounded queue. 1 = no queueing at all
     *  (the consumer always sees the newest frame). */
    std::uint32_t queue_depth = 2;

    /** The overflow policy. */
    CaptureDropPolicy drop_policy = CaptureDropPolicy::kDropOldest;

    /** Whether the caller wants the pointer composited into the frame.
     *  Honoured only if the device can do it — check
     *  `CaptureCapabilities::cursor_composited`. */
    bool want_cursor = true;

    /**
     * @brief Whether the caller wants the zero-copy path (plan B10).
     *
     * A zero-copy device hands the captured picture to the consumer as
     * a **GPU surface** instead of CPU pixels: the hardware encoder
     * takes the native handle directly, and no GPU→CPU readback happens.
     *
     * The request is a *request*, not a command: the device reports what
     * it actually did through `CaptureFormat::zero_copy` and through the
     * `gpu_surface` field of every frame it delivers.  A device that
     * cannot do it (no hardware encoder on the other end, a cursor layer
     * that needs CPU pixels, a backend without a GPU surface) falls
     * back to the CPU path and says so.
     *
     * Two consequences a host must know about:
     *
     *   - the device can no longer composite the pointer into the frame
     *     (there are no CPU pixels to draw into), so a host that wants
     *     both has to pick one;
     *   - the frames that arrive carry no pixel bytes, so a consumer
     *     that does not support the zero-copy path cannot use them.
     */
    bool want_zero_copy = false;

    /** The captured surface's origin in the platform's *global* pointer
     *  coordinate space.
     *
     *  A pointer is reported in the platform's global space — the
     *  virtual desktop on Windows, the root window on X11 — while a
     *  captured frame is one *surface* within that space, and a surface
     *  is not necessarily at the global origin (a second monitor sits
     *  to the right of the first).  The device fills these two fields
     *  in when it opens, so the cursor layer can translate the pointer
     *  into the frame's own coordinates.
     *
     *  Both are zero for a capture that starts at the global origin,
     *  and for a device whose surface *is* the global space.
     */
    std::int32_t surface_origin_x = 0;
    std::int32_t surface_origin_y = 0;
};

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

/** Capture-side counters, surfaced through the engine's stats snapshot
 *  (remote-desktop plan A11). */
struct NIMRTC_API_STABLE CaptureStats {
    /** Frames delivered to the consumer. */
    std::uint64_t frames_captured = 0;
    /** Frames dropped because the queue was full. */
    std::uint64_t frames_dropped = 0;
    /** Frames the device could not deliver (device lost, format change). */
    std::uint64_t device_errors = 0;
    /** Times the device was lost and had to be reopened. */
    std::uint64_t device_restarts = 0;
    /** Number of times the negotiated format changed mid-stream. */
    std::uint64_t format_changes = 0;
    /** Bytes copied CPU↔GPU (0 when the path was fully zero-copy). */
    std::uint64_t bytes_copied = 0;
};

// ---------------------------------------------------------------------------
// Frame delivery
// ---------------------------------------------------------------------------

/**
 * @brief One captured frame.
 *
 * The `pixels` pointer is owned by the capture instance and stays valid
 * **until the next call to `pop_frame()`**.  The consumer must copy
 * anything it wants to keep — or, on the zero-copy path, it must hand the
 * frame to a hardware encoder that consumes it within the same call.
 */
struct NIMRTC_API_STABLE CaptureFrame {
    /** Pointer to the pixel data. Null when the frame carries only
     *  metadata (e.g. a dirty-rect-only notification). */
    const std::uint8_t* pixels = nullptr;

    /** Size of the pixel data in bytes. */
    std::size_t size = 0;

    /** Stride (bytes per row) of the pixel data. */
    std::int32_t stride = 0;

    /** Width in pixels. */
    std::uint32_t width = 0;
    /** Height in pixels. */
    std::uint32_t height = 0;

    /** Monotonic capture timestamp, microseconds. */
    std::int64_t capture_ts_us = 0;

    /** Per-device frame sequence number (strictly increasing). */
    std::uint32_t frame_seq = 0;

    /** The changed region(s) in this frame. */
    CaptureDirtyRects dirty;

    /** The colour description of the picture (remote-desktop plan C5).
     *
     *  A desktop compositor produces **full-range BT.709**; a camera
     *  produces **limited-range BT.601**.  The device states what it
     *  actually produces, and the adapter converts the pixels with that
     *  description — so the encoder receives a picture whose luma and
     *  chroma scaling it can trust.
     *
     *  `kUnspecified` on both fields means "the device did not say", and
     *  the portable layer resolves that to the desktop default (full
     *  range, BT.709). */
    video_frame::ColorDescription color{};

    /** True iff this frame carries a GPU handle that a zero-copy
     *  encoder can consume directly. */
    bool has_gpu_handle = false;

    // ---- Zero-copy surface (remote-desktop plan B10) ----------------------
    //
    // The GPU half of a captured frame.  When `gpu_surface` is set the
    // frame's pixels live in GPU memory and `pixels` may be null: the
    // surface *is* the picture, and no CPU readback has happened (or
    // needs to).
    //
    // The handle is **borrowed**, not owned: it belongs to the capture
    // device and stays valid until the device's next frame.  The
    // consumer that wants to keep the surface past the current call
    // must copy it (or ask the device for a persistent surface); the
    // zero-copy path hands it straight to the hardware encoder within
    // the call, which is the whole point of the path.
    //
    // The `device` token in the handle identifies the GPU device the
    // surface belongs to — an encoder session is opened against one
    // device, so producer and consumer must agree.
    GpuHandle gpu_surface{};
};

/**
 * @brief Frame-delivery callback.
 *
 * Invoked on the capture plug-in's own thread when a frame arrives.  It
 * must not block: the implementation enqueues and returns.
 */
using NIMRTC_API_STABLE CaptureFrameCallback =
    std::function<void(const CaptureFrame& frame)>;

// ---------------------------------------------------------------------------
// IVideoCapture
// ---------------------------------------------------------------------------

/**
 * @brief A capture *device*.
 *
 * Thread safety: the enumeration and query methods are const and safe to
 * call concurrently; `open()` / `close()` / `pop_frame()` must be
 * serialized externally.
 */
class NIMRTC_API_STABLE IVideoCapture : public IPlugin {
public:
    // ---- Enumeration ------------------------------------------------------

    /**
     * List the devices this backend can capture.
     *
     * On Wayland the *portal* owns source selection, so this list is
     * whatever the portal has already granted — the engine cannot
     * enumerate monitors it was not given.  That is a platform
     * limitation, not a bug (remote-desktop plan A5).
     */
    [[nodiscard]] virtual std::vector<CaptureDevice> enumerate_devices()
        const noexcept = 0;

    /** The formats @p device can deliver. */
    [[nodiscard]] virtual std::vector<CaptureFormat> supported_formats(
        const std::string& device_id) const noexcept = 0;

    /** What this backend can do. */
    [[nodiscard]] virtual CaptureCapabilities capabilities() const noexcept = 0;

    // ---- Lifecycle --------------------------------------------------------

    /**
     * Open the device and begin capturing.
     *
     * The configuration given to the factory at creation time is the
     * *request*; after `open()` succeeds, `config()` reports the format
     * that was actually negotiated — the caller must read it back rather
     * than assume the request was granted (a device may have to fall
     * back to a smaller size or a lower frame rate).
     */
    virtual Status open() noexcept = 0;

    /** Stop capturing and release the device. Idempotent. */
    virtual void close() noexcept = 0;

    /** True iff the device is currently open. */
    [[nodiscard]] virtual bool is_open() const noexcept = 0;

    /** The configuration actually in force (post-negotiation). */
    [[nodiscard]] virtual CaptureConfig config() const noexcept = 0;

    // ---- Frame delivery ---------------------------------------------------

    /**
     * Pop the next frame from the bounded queue.
     *
     * @param out  Receives the frame. The pixels are valid only until the
     *             next call to `pop_frame()`.
     * @return kOk when a frame was delivered; `kErrNotReady` when the
     *         queue is empty.
     */
    virtual Status pop_frame(CaptureFrame& out) noexcept = 0;

    /** Called by the capture plug-in's own thread when a frame arrives.
     *  Optional: a consumer that only polls `pop_frame()` need not set
     *  it. */
    virtual void set_frame_callback(CaptureFrameCallback cb) noexcept = 0;

    /** Counters. */
    [[nodiscard]] virtual CaptureStats stats() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

/** Factory for capture-device plugin instances. */
class NIMRTC_API_STABLE IVideoCaptureFactory {
public:
    virtual ~IVideoCaptureFactory() = default;

    /** Unique identifier, e.g. "dxgi_desktop", "screen_capture_kit",
     *  "x11", "synthetic". */
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;

    /** Short human-readable name. */
    [[nodiscard]] virtual std::string_view display_name() const noexcept = 0;

    /** Create a capture instance. The returned instance is in
     *  kConstructed state; the caller calls `open()`. */
    [[nodiscard]] virtual IVideoCapture* create(CaptureConfig cfg) const = 0;

    /**
     * @brief Create the *source* that presents @p device as a frame stream.
     *
     * The engine's video path speaks `IVideoSource` — a plain stream of
     * `VideoSourceFrame`s — while a capture device is a *device*: it
     * enumerates, negotiates a format and fills a bounded queue.  The
     * piece that bridges the two is a *concrete* adapter, and it lives
     * in the capture module.
     *
     * The engine must not name that adapter: it reaches it through this
     * seam instead.  That keeps the engine free of any concrete
     * module include (ADR-025's Layering Invariant 4) while still
     * letting the capture module own the conversion from the device's
     * layout into the source's layout.
     *
     * The returned source does **not** own @p device: the caller keeps
     * the device alive for at least as long as the source is used, so
     * that one device can back more than one stream.
     *
     * @param device The device to present.  Must not be null.
     * @param cfg    The source configuration.  `cfg.format` is the
     *               *output* pixel format; the adapter fills the
     *               dimensions in from the device's negotiated format.
     * @return A new source, or null when one could not be created.  The
     *         caller owns it.
     */
    [[nodiscard]] virtual IVideoSource* create_source(
        IVideoCapture* device,
        VideoSourceConfig cfg) const = 0;
};

} // namespace nimrtc::plugins
#endif // NIMRTC_PLUGINS_VIDEO_CAPTURE_HPP
