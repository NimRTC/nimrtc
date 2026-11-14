/**
 * @file nimrtc/video_capture/cursor_compositor.hpp
 * @brief The pluggable cursor-compositing layer (remote-desktop plan A6).
 *
 * Some desktop compositors draw the pointer into the frame they hand
 * over, and some do not.  ScreenCaptureKit composites the cursor; the
 * Windows DXGI Desktop Duplication path does **not** — it delivers the
 * pixels of the display with the pointer left out, because the pointer
 * is a separate hardware overlay.
 *
 * So the pointer is a *layer*, not a property of a frame:
 *
 * @code
 *   platform cursor source  ──► CursorImage  ──► CursorCompositor ──► frame
 *   (GetCursorInfo / XFixes)     (bitmap +       (alpha blend)
 *                                 hotspot)
 * @endcode
 *
 * The layer is **pluggable**: a capture backend that reports
 * `CaptureCapabilities::cursor_composited == false` can still deliver a
 * cursor-composited picture when a cursor source is attached to it.  The
 * capability flag stays honest — it says what the *device* does — and
 * the compositor is the separate layer the plan describes.
 *
 * ## What is portable
 *
 * Everything in this header except the *cursor sources* is portable
 * C++20 over raw bytes, and it is therefore unit-tested on **every** CI
 * leg, including the Windows leg where the macOS and Linux adapters
 * cannot even be compiled:
 *
 *   - the alpha blend (a real source-over blend of the cursor bitmap
 *     over the frame's BGRA pixels, honouring the hotspot and the
 *     frame's stride),
 *   - the clipping (a cursor near the frame's edge is *clipped*, never
 *     written out of bounds),
 *   - the **recoverability** of the raw frame (the compositor keeps a
 *     copy of the region it touched, so the original pixels can be
 *     restored — compositing is therefore not a one-way door),
 *   - and the screen → frame coordinate translation.
 *
 * The *cursor sources* — Windows `GetCursorInfo` + `DrawIconEx`, X11
 * `XFixesGetCursorImage` — are thin platform adapters that produce a
 * `CursorImage`.  macOS needs none: ScreenCaptureKit composites the
 * pointer itself.
 *
 * @note Remote-desktop plan A6, milestone M1.
 */

#ifndef NIMRTC_VIDEO_CAPTURE_CURSOR_COMPOSITOR_HPP
#define NIMRTC_VIDEO_CAPTURE_CURSOR_COMPOSITOR_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <nimrtc/plugins/video_capture.hpp>

namespace nimrtc::video_capture {

// The cursor *source* seam lives in its own header; it is forward-
// declared here so that the compositor can be used without it, and so
// that the two headers can refer to each other.
class NIMRTC_API_STABLE ICursorSource;

// ---------------------------------------------------------------------------
// CursorImage
// ---------------------------------------------------------------------------

/**
 * @brief The cursor's bitmap and its hotspot.
 *
 * A platform backend produces one of these; the compositor blends it
 * into the frame.
 *
 * The bitmap is **32-bit BGRA** — the same layout the desktop
 * compositors produce — so the blend needs no conversion.  The alpha
 * byte is the cursor's own coverage: 0 = fully transparent, 255 =
 * fully opaque.
 *
 * The **hotspot** is the point *inside* the bitmap that corresponds to
 * the pointer's actual position (the tip of the arrow).  The compositor
 * places the bitmap so that the hotspot lands on the pointer's
 * position, which is why a cursor drawn at the screen's top-left corner
 * is still fully visible.
 */
struct NIMRTC_API_STABLE CursorImage {
    /** The bitmap's width in pixels. */
    std::uint32_t width = 0;

    /** The bitmap's height in pixels. */
    std::uint32_t height = 0;

    /** The hotspot's x offset inside the bitmap, in pixels. */
    std::int32_t hotspot_x = 0;

    /** The hotspot's y offset inside the bitmap, in pixels. */
    std::int32_t hotspot_y = 0;

    /** The bitmap's row stride in bytes. 0 means "packed" (`width * 4`). */
    std::int32_t stride = 0;

    /** The bitmap's pixels: BGRA, `height` rows of `stride` bytes.
     *
     *  The last row's padding bytes (when `stride > width * 4`) are
     *  ignored. */
    std::vector<std::uint8_t> pixels;

    /** True iff the shape carries a usable bitmap. */
    [[nodiscard]] bool valid() const noexcept {
        if (width == 0 || height == 0) return false;
        return pixels.size() >= required_bytes();
    }

    /** The row stride in bytes, resolving the "packed" case. */
    [[nodiscard]] std::int32_t row_stride() const noexcept {
        if (stride > 0) return stride;
        return static_cast<std::int32_t>(width * 4u);
    }

    /** The number of bytes a valid bitmap needs. */
    [[nodiscard]] std::size_t required_bytes() const noexcept {
        const std::int32_t s = row_stride();
        if (s <= 0) return 0;
        return static_cast<std::size_t>(s) * height;
    }
};

// ---------------------------------------------------------------------------
// The coordinate translation
// ---------------------------------------------------------------------------

/**
 * @brief A cursor position, in frame coordinates.
 */
struct NIMRTC_API_STABLE CursorPosition {
    /** The position, in frame pixels.  It may be negative (the pointer
     *  is off the captured surface, to the left or above). */
    std::int32_t x = 0;
    std::int32_t y = 0;

    /** True iff the position is inside the captured surface. */
    bool inside = false;
};

// ---------------------------------------------------------------------------
// CursorCompositor
// ---------------------------------------------------------------------------

/**
 * @brief Blends a cursor bitmap into a BGRA frame.
 *
 * The compositor is **portable**: it works on a plain BGRA buffer with
 * a row stride, and it never touches an OS API.
 *
 * ## The blend
 *
 * The cursor's bitmap is blended over the frame with a *source-over*
 * alpha blend, using the bitmap's own alpha byte:
 *
 * @code
 *   out = (src * a + dst * (255 - a) + 127) / 255
 * @endcode
 *
 * The arithmetic is exact at the extremes: `a == 0` leaves the
 * destination byte **bit-identical**, and `a == 255` replaces it with
 * the source byte.  The destination's alpha byte is left untouched —
 * the captured frames are opaque by construction, and leaving it alone
 * is what makes "a fully transparent cursor changes nothing" true.
 *
 * ## The recoverability
 *
 * Compositing is *not* a one-way door.  Before the compositor writes
 * anything it keeps a copy of the region it is about to touch, so
 * `restore()` puts the original pixels back.  That is what makes the
 * operation idempotent: composite → restore → composite produces
 * exactly the same picture as composite → composite would *not*.
 *
 * ## The clipping
 *
 * A cursor near the frame's edge is **clipped**, not written out of
 * bounds: only the part of the bitmap that lands inside the frame is
 * blended, and the blend reads only the bitmap's own bytes.  The
 * result reports the bounding box that was actually touched, so a
 * caller can add it to the frame's dirty rectangles.
 */
class NIMRTC_API_STABLE CursorCompositor {
public:
    /** The outcome of one composite operation. */
    struct NIMRTC_API_STABLE CompositeResult {
        /** True iff at least one pixel was blended. */
        bool applied = false;

        /** The bounding box that was touched, in frame pixels.
         *
         *  It is empty when nothing was applied. */
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;

        /** True iff the result touches no pixel. */
        [[nodiscard]] bool empty() const noexcept { return !applied; }
    };

    CursorCompositor() = default;

    // ---- The blend --------------------------------------------------------

    /**
     * @brief Blend @p image into @p frame at the pointer's position.
     *
     * @param image   The cursor's bitmap and hotspot.
     * @param x       The pointer's x position, in **frame** pixels.
     * @param y       The pointer's y position, in **frame** pixels.
     * @param frame   The BGRA frame.  Modified in place.
     * @param size    The size of @p frame in bytes.
     * @param stride  The frame's row stride in bytes.  0 means "packed".
     * @param width   The frame's width in pixels.
     * @param height  The frame's height in pixels.
     * @return The bounding box that was touched.
     */
    CompositeResult composite(const CursorImage& image, std::int32_t x, std::int32_t y,
                              std::uint8_t* frame, std::size_t size, std::int32_t stride,
                              std::uint32_t width, std::uint32_t height) noexcept;

    /**
     * @brief Restore the frame to its state before the last blend.
     *
     * Only the region the last blend touched is restored, so the rest
     * of the frame is left exactly as it was.
     */
    void restore() noexcept;

    /** True iff there is a blend that `restore()` can undo. */
    [[nodiscard]] bool can_restore() const noexcept { return has_backup_; }

    // ---- The coordinate translation ---------------------------------------

    /**
     * @brief Translate a screen position into frame coordinates.
     *
     * The cursor sources report the pointer's position in **screen**
     * coordinates (that is what `GetCursorInfo` and `XQueryPointer`
     * give).  The compositor needs it in the captured surface's own
     * coordinate space, which starts at the origin of the captured
     * display or window.
     *
     * @param screen_x    The pointer's x position, in screen pixels.
     * @param screen_y    The pointer's y position, in screen pixels.
     * @param origin_x    The captured surface's x origin, in screen pixels.
     * @param origin_y    The captured surface's y origin, in screen pixels.
     * @param width       The captured surface's width, in pixels.
     * @param height      The captured surface's height, in pixels.
     */
    [[nodiscard]] static CursorPosition screen_to_frame(std::int32_t screen_x,
                                                        std::int32_t screen_y,
                                                        std::int32_t origin_x,
                                                        std::int32_t origin_y,
                                                        std::uint32_t width,
                                                        std::uint32_t height) noexcept;

private:
    /** The copy of the region the last blend touched. */
    std::vector<std::uint8_t> backup_{};
    std::uint8_t* backup_frame_ = nullptr;
    std::int32_t backup_stride_ = 0;
    std::uint32_t backup_x_ = 0;
    std::uint32_t backup_y_ = 0;
    std::uint32_t backup_width_ = 0;
    std::uint32_t backup_height_ = 0;
    bool has_backup_ = false;
};

// ---------------------------------------------------------------------------
// CursorCompositingCapture
// ---------------------------------------------------------------------------

/**
 * @brief A capture decorator that composites the cursor into the frames.
 *
 * This is the "wiring" of plan A6: it takes any `plugins::IVideoCapture`
 * — the DXGI path, the X11 path, the synthetic device — and delivers
 * cursor-composited frames from it.  The wrapped device's own
 * `CaptureCapabilities::cursor_composited` flag is left **honest**: it
 * still says what the *device* does, and the decorator is the separate
 * layer that adds the pointer.
 *
 * The decorator copies the frame before blending it, because the
 * `CaptureFrame` contract hands out a *borrowed* pointer into the
 * device's own buffer; the copy is what makes the blend legal.
 *
 * Frames whose pixel format is not BGRA pass through **unchanged** — a
 * camera (NV12) or an I420 screen capture is not a canvas the cursor
 * can be blended into, and pretending otherwise would corrupt it.
 */
class NIMRTC_API_STABLE CursorCompositingCapture final : public plugins::IVideoCapture {
public:
    /**
     * @brief Wrap a capture device with the cursor layer.
     *
     * @param inner  The device to wrap.  Ownership is taken.
     * @param source The platform cursor source.  May be null, in which
     *               case only the pinned fallback is used.
     */
    explicit CursorCompositingCapture(std::unique_ptr<plugins::IVideoCapture> inner,
                                      std::shared_ptr<ICursorSource> source);

    ~CursorCompositingCapture() override;

    CursorCompositingCapture(const CursorCompositingCapture&) = delete;
    CursorCompositingCapture& operator=(const CursorCompositingCapture&) = delete;

    // ---- IPlugin ----------------------------------------------------------
    [[nodiscard]] const char* name() const noexcept override;

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

    // ---- Cursor-specific --------------------------------------------------

    /** The compositor this decorator uses. */
    [[nodiscard]] CursorCompositor& compositor() noexcept { return compositor_; }

    /** The compositor this decorator uses. */
    [[nodiscard]] const CursorCompositor& compositor() const noexcept {
        return compositor_;
    }

private:
    std::unique_ptr<plugins::IVideoCapture> inner_;
    std::shared_ptr<ICursorSource> source_;
    /**
     * The pinned fallback wrapping `source_` (created on first use).
     *
     * It is held as a `shared_ptr<ICursorSource>` because the
     * fallback's own type lives in the *source* seam's header, which
     * includes this one — the dependency cannot be inverted.
     */
    std::shared_ptr<ICursorSource> pinned_;
    CursorCompositor compositor_;

    /** The copy of the frame the blend is applied to. */
    std::vector<std::uint8_t> scratch_{};
};

} // namespace nimrtc::video_capture
#endif // NIMRTC_VIDEO_CAPTURE_CURSOR_COMPOSITOR_HPP
