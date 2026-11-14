/**
 * @file nimrtc/video_frame/roi.hpp
 * @brief Scaling a source-space rectangle into an output resolution.
 *
 * ## Why this lives here
 *
 * A remote-desktop session does not stay at one resolution: the rate
 * controller's ladder (remote-desktop plan D2) steps the encoder between
 * resolutions as the link changes.  A rectangle expressed in *capture*
 * pixels therefore has to be rescaled at every ladder step, and getting
 * that wrong shows up as "the cursor region is blurry" rather than as a
 * crash.
 *
 * So the rectangle is expressed once, in capture coordinates, and **one**
 * place knows how to carry it across a resolution change.  This file is
 * that place.
 *
 * It lives in `video_frame` rather than in a higher layer because it is
 * pure pixel geometry — it knows nothing about codecs, encoders, the
 * engine or the network.  Both the encoder-side policy
 * (`nimrtc::video_pipeline::RoiScaler`, plan C4) and the engine's
 * parameter computation use it, and the engine is allowed to reach
 * `video_frame` (ADR-025's Layering Invariant 4 exemption list).
 *
 * ## The rule
 *
 * ```
 *   out_x = round(src_x * out_w  / capture_w)
 *   out_y = round(src_y * out_h  / capture_h)
 *   out_w = round(src_w * out_w  / capture_w)
 *   out_h = round(src_h * out_h  / capture_h)
 * ```
 *
 * The result is **clamped** to the output frame.  A rectangle that
 * collapses to zero width or height after scaling is dropped rather than
 * handed on as a degenerate rectangle — a degenerate rectangle would
 * either be rejected by the codec or, worse, silently disable the
 * mechanism.
 *
 * @note Remote-desktop plan C4 (the shared geometry rule).
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md). Per-symbol sweep is
// a v1.13.x follow-up.
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <optional>

namespace nimrtc::video_frame {

// ---------------------------------------------------------------------------
// ScaledRect
// ---------------------------------------------------------------------------

/** A rectangle in output (encoder) pixels. */
struct NIMRTC_API_STABLE ScaledRect {
    std::uint32_t x      = 0;
    std::uint32_t y      = 0;
    std::uint32_t width  = 0;
    std::uint32_t height = 0;

    /** True iff the rectangle covers no pixels at all. */
    [[nodiscard]] bool empty() const noexcept {
        return width == 0 || height == 0;
    }
};

// ---------------------------------------------------------------------------
// scale_region
// ---------------------------------------------------------------------------

/**
 * @brief Scale a source-space rectangle into an output resolution.
 *
 * @param src_x        the rectangle's x in source pixels.
 * @param src_y        the rectangle's y in source pixels.
 * @param src_width    the rectangle's width in source pixels.
 * @param src_height   the rectangle's height in source pixels.
 * @param capture_w    the capture resolution's width.
 * @param capture_h    the capture resolution's height.
 * @param out_w        the output resolution's width.
 * @param out_h        the output resolution's height.
 * @param min_size     the smallest rectangle (in output pixels) worth
 *                     keeping.  Zero disables the floor.
 * @return the scaled, clamped rectangle, or `std::nullopt` when the
 *         inputs are degenerate, the rectangle collapses, or it falls
 *         below `min_size`.
 */
NIMRTC_API_STABLE std::optional<ScaledRect>
scale_region(std::uint32_t src_x,
             std::uint32_t src_y,
             std::uint32_t src_width,
             std::uint32_t src_height,
             std::uint32_t capture_w,
             std::uint32_t capture_h,
             std::uint32_t out_w,
             std::uint32_t out_h,
             std::uint32_t min_size = 0) noexcept;

} // namespace nimrtc::video_frame
