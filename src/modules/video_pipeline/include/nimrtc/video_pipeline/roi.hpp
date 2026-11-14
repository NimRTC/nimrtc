/**
 * @file nimrtc/video_pipeline/roi.hpp
 * @brief The region-of-interest (remote-desktop plan C4).
 *
 * ## What this is
 *
 * Plan C4: "the encoder may be given a sub-rectangle to code at higher
 * fidelity (the cursor, the region the operator is looking at).  The ROI
 * is a rectangle in **capture coordinates**, scaled with the frame."
 *
 * The important half of that sentence is the last one.  A remote-desktop
 * session does not stay at one resolution: the rate controller's ladder
 * (plan D2) steps the encoder between resolutions as the link changes.
 * A ROI expressed in capture pixels would therefore have to be rescaled
 * at every ladder step — and getting that wrong is exactly the kind of
 * bug that shows up as "the cursor region is blurry" rather than as a
 * crash.
 *
 * So the ROI is expressed once, in **capture coordinates**, and this
 * class is the single place that knows how to carry it across a
 * resolution change.  The encoder only ever sees the rectangle in
 * **output pixels**, so the encoder needs to know nothing about the
 * capture resolution.
 *
 * ## What a ROI is *not*
 *
 * ADR-022 is explicit: the ROI is *not* the capture region.  The capture
 * region says *what* to capture; the ROI says *where to spend the bits*.
 * This class therefore takes a capture-space rectangle and never touches
 * the capture device.
 *
 * ## Threading
 *
 * Not thread-safe.  One instance per outbound video stream.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v1.13.x follow-up.
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <optional>

#include <nimrtc/plugins/video_codec.hpp>

namespace nimrtc::video_pipeline {

// ---------------------------------------------------------------------------
// RoiScaler
// ---------------------------------------------------------------------------

/**
 * @brief Carries a capture-space ROI across resolution changes.
 *
 * ## The scaling rule
 *
 * The ROI is scaled by the ratio between the *output* resolution and the
 * *capture* resolution, independently for x and y:
 *
 * ```
 *   out_x = round(capture_x * out_width  / capture_width)
 *   out_y = round(capture_y * out_height / capture_height)
 *   out_w = round(capture_w * out_width  / capture_width)
 *   out_h = round(capture_h * out_height / capture_height)
 * ```
 *
 * The result is **clamped** to the output frame, and a rectangle that
 * collapses to zero width or height after scaling is dropped (reported
 * as "no ROI") rather than handed to the encoder as a degenerate
 * rectangle.  A degenerate ROI would either be rejected by the codec or
 * — worse — silently disable the mechanism.
 *
 * ## The minimum size
 *
 * A ROI smaller than a macroblock is not worth asking for: the encoder
 * cannot honour it, and asking makes the encoder's behaviour
 * implementation-defined.  `min_size` (default 16 px) is the floor; a
 * smaller ROI is dropped and reported.
 */
class NIMRTC_API_STABLE RoiScaler {
public:
    /**
     * @brief Construct a scaler.
     *
     * @param capture_width  the capture resolution's width.
     * @param capture_height the capture resolution's height.
     * @param min_size      the smallest ROI (in output pixels) worth
     *                       asking for.  Zero disables the floor.
     */
    RoiScaler(std::uint32_t capture_width,
              std::uint32_t capture_height,
              std::uint32_t min_size = 16);

    /**
     * @brief Set (or clear) the ROI, in capture coordinates.
     *
     * Passing `std::nullopt` clears the ROI.
     */
    void set_roi(std::optional<plugins::VideoRegion> roi) noexcept;

    /** The current ROI, in capture coordinates. */
    const std::optional<plugins::VideoRegion>& roi() const noexcept { return roi_; }

    /** True iff a ROI is set. */
    bool has_roi() const noexcept { return roi_.has_value(); }

    /**
     * @brief Scale the ROI into an output resolution.
     *
     * @param out_width  the encoder's target width.
     * @param out_height the encoder's target height.
     * @return the ROI in output pixels, or `std::nullopt` when there is
     *         no ROI, when the ROI collapses, or when it is below the
     *         minimum size.
     */
    std::optional<plugins::VideoRegion> scale(std::uint32_t out_width,
                                               std::uint32_t out_height) const noexcept;

    /**
     * @brief The ROI in output pixels for a given resolution, cached.
     *
     * The engine calls `scale()` on every ladder step, not on every
     * frame, so the caching matters less than the clarity — but the
     * cache makes the "the ROI did not change" case free, which is the
     * common case for a session whose resolution is stable.
     */
    std::optional<plugins::VideoRegion> scaled(std::uint32_t out_width,
                                                std::uint32_t out_height) noexcept;

    /** The capture resolution this scaler was built for. */
    std::uint32_t capture_width() const noexcept { return capture_width_; }
    std::uint32_t capture_height() const noexcept { return capture_height_; }

    /** The smallest ROI (in output pixels) worth asking for. */
    std::uint32_t min_size() const noexcept { return min_size_; }

    /**
     * @brief The number of times the ROI was dropped because it was
     *        degenerate or too small.
     *
     * The counter is what proves the mechanism is honest: an ROI that
     * silently stops being honoured must be visible.
     */
    std::uint64_t dropped_count() const noexcept { return dropped_; }

    /** Forget the cached scaling and the dropped counter. */
    void reset() noexcept;

private:
    std::uint32_t capture_width_;
    std::uint32_t capture_height_;
    std::uint32_t min_size_;

    std::optional<plugins::VideoRegion> roi_;

    // The cached scaling.
    bool cache_valid_ = false;
    std::uint32_t cache_width_ = 0;
    std::uint32_t cache_height_ = 0;
    std::optional<plugins::VideoRegion> cache_value_;

    std::uint64_t dropped_ = 0;
};

} // namespace nimrtc::video_pipeline
