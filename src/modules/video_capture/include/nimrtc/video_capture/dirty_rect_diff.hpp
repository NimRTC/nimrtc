/**
 * @file nimrtc/video_capture/dirty_rect_diff.hpp
 * @brief The portable dirty-rectangle detector (remote-desktop plan A7).
 *
 * ## Why this exists
 *
 * DXGI Desktop Duplication reports the changed regions of a frame
 * *natively* — the compositor already knows what it redrew, and it
 * hands the rectangles over.  Every other backend does not:
 *
 *   - **X11** (`XGetImage`) gives the whole root window's pixels and no
 *     damage information at all;
 *   - **PipeWire / the XDG portal** gives the frames a compositor
 *     decided to send, but no per-region damage;
 *   - **Windows Graphics Capture** and **ScreenCaptureKit** hand over
 *     whole surfaces.
 *
 * Without damage information, a *static* desktop produces a full frame
 * every single time — and a full frame at 4K is the single most
 * expensive thing in the whole pipeline (a full CPU readback plus a
 * full colour conversion plus a full encode).  The plan's §B.6 budget
 * makes that unaffordable.
 *
 * This file provides the *portable* answer: compare the new frame with
 * the previous one and report the region that changed.  It is
 * deliberately a **bounding box** rather than an exact set of
 * rectangles: the encoder's job is to know *where* to look, and one
 * rectangle is enough for the region-invalidation mechanism (plan B9)
 * and for the "skip the unchanged part" optimisation.
 *
 * ## Where the diff runs
 *
 * The diff is a property of the *capture layer*, not of a platform:
 * every backend that cannot report damage natively runs it.  The
 * detector is therefore a class the backend owns, and the backend calls
 * it after it has the new pixels and before it queues the frame.
 *
 * ## The cost
 *
 * The diff is one pass over the frame comparing 32-bit pixels — a
 * `memcmp`-shaped loop, which the compiler vectorises.  It is *cheaper*
 * than the readback it replaces (the pixels are already in RAM) and
 * *far* cheaper than the colour conversion and the encode it saves.
 *
 * The diff can also be done at a *coarser* granularity: comparing
 * every Nth pixel and then expanding the resulting box is enough for
 * screen content, where the changed region is usually large and
 * axis-aligned.  `stride_pixels` is that knob.
 *
 * @note Remote-desktop plan A7.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <nimrtc/plugins/video_capture.hpp>

namespace nimrtc::video_capture {

/**
 * @brief Detects the changed region between two consecutive frames.
 *
 * The detector keeps the *previous* frame's bytes so that the next
 * frame can be compared against it.  The memory it holds is one frame
 * — for a 4K BGRA desktop that is 33 MiB, which is the price of the
 * optimisation.  A host that cannot afford it can leave the detector
 * out and accept the full-frame cost (the backend's own
 * `has_dirty_rects` flag then reports the truth).
 */
class NIMRTC_API_STABLE DirtyRectDetector {
public:
    /** Construct a detector.
     *
     * @param stride_pixels How many pixels to skip between two
     *        compared samples.  1 means "compare every pixel" (exact);
     *        4 is a good compromise for screen content, where the
     *        changed region is large and axis-aligned.  The result is
     *        always *expanded* to the block the sample belongs to, so
     *        the reported rectangle never under-covers a change.
     */
    explicit DirtyRectDetector(std::uint32_t stride_pixels = 1) noexcept
        : stride_(stride_pixels == 0 ? 1u : stride_pixels) {}

    /** The sampling stride. */
    [[nodiscard]] std::uint32_t stride_pixels() const noexcept { return stride_; }

    /** Forget the previous frame, so the next comparison reports a
     *  full-frame change.  Called when the geometry changes or the
     *  device is reopened. */
    void reset() noexcept;

    /**
     * Compare @p pixels against the previous frame and report what
     * changed.
     *
     * @param pixels  The new frame's bytes.
     * @param size    Their size in bytes.
     * @param stride  The row stride in bytes.
     * @param width   The frame width in pixels.
     * @param height  The frame height in pixels.
     * @param bytes_per_pixel How many bytes one pixel occupies (4 for
     *        BGRA / RGBA, 1 for a greyscale plane).
     * @param out     The changed region(s).  Filled in place.
     *
     * @return true when the frame is *identical* to the previous one
     *         (in which case `out` is left with an empty rectangle
     *         list and `full_frame` false).  false when something
     *         changed.
     */
    bool compare(const std::uint8_t* pixels,
                 std::size_t size,
                 std::int32_t stride,
                 std::uint32_t width,
                 std::uint32_t height,
                 std::uint32_t bytes_per_pixel,
                 plugins::CaptureDirtyRects& out) noexcept;

    /** True iff a previous frame is being remembered (i.e. the next
     *  comparison can detect a change). */
    [[nodiscard]] bool has_previous() const noexcept { return has_previous_; }

private:
    /** Copy the frame into the packed `previous_` buffer and record the
     *  geometry it had. */
    void remember(const std::uint8_t* pixels,
                  std::int32_t row_stride,
                  std::size_t row_bytes,
                  std::uint32_t width,
                  std::uint32_t height,
                  std::uint32_t bytes_per_pixel) noexcept;

    std::uint32_t stride_ = 1;

    /** The previous frame's bytes. */
    std::vector<std::uint8_t> previous_;

    /** The geometry the previous frame had.  A geometry change makes
     *  the comparison meaningless, so the next frame is reported as a
     *  full change. */
    std::uint32_t prev_width_  = 0;
    std::uint32_t prev_height_ = 0;
    std::int32_t  prev_stride_ = 0;
    std::uint32_t prev_bpp_    = 0;

    bool has_previous_ = false;
};

} // namespace nimrtc::video_capture
