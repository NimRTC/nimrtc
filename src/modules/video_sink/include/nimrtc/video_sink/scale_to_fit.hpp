/**
 * @file nimrtc/video_sink/scale_to_fit.hpp
 * @brief The portable scale-to-fit geometry helper (remote-desktop plan E7).
 *
 * ## What this is
 *
 * Plan E7 asks for a *full-screen / scale-to-fit* presentation: the
 * decoded picture is fitted into the viewer's window **honouring the
 * aspect ratio**, so a 16:9 desktop shown in a 4:3 window is letterboxed
 * rather than stretched.
 *
 * The geometry rule is pure arithmetic — it depends on nothing but the
 * source and target sizes — so it lives in a header-only, platform-neutral
 * helper that is unit-tested on every platform.
 *
 * ## The three modes
 *
 *   - **kLetterbox** — fit the whole source *inside* the target, keeping
 *     the aspect ratio.  The result is smaller than the target in one
 *     dimension and centred in the other; the uncovered area is black.
 *   - **kCrop** — fill the whole target, keeping the aspect ratio.  The
 *     result covers the target exactly and the overflow on one axis is
 *     cropped away.
 *   - **kStretch** — fill the whole target, ignoring the aspect ratio.
 *     The picture is distorted.  It exists because some hosts want it
 *     (a deliberately distorted preview is sometimes useful), not because
 *     it is correct.
 *
 * @note Remote-desktop plan E7.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstdint>

namespace nimrtc::video_sink {

// ---------------------------------------------------------------------------
// ScaleMode
// ---------------------------------------------------------------------------

/** How a source picture is fitted into a target rectangle. */
enum class NIMRTC_API_STABLE ScaleMode : std::uint8_t {
    /** Fit inside the target, preserving the aspect ratio (bars on the
     *  short axis). */
    kLetterbox = 0,
    /** Fill the target, preserving the aspect ratio (overflow cropped). */
    kCrop      = 1,
    /** Fill the target, ignoring the aspect ratio (the picture is
     *  distorted). */
    kStretch   = 2,
};

// ---------------------------------------------------------------------------
// ScaleRect
// ---------------------------------------------------------------------------

/** A destination rectangle in target-pixel coordinates. */
struct NIMRTC_API_STABLE ScaleRect {
    /** The left edge. */
    std::int32_t x = 0;
    /** The top edge. */
    std::int32_t y = 0;
    /** The width. */
    std::int32_t w = 0;
    /** The height. */
    std::int32_t h = 0;
};

// ---------------------------------------------------------------------------
// compute_scale_to_fit
// ---------------------------------------------------------------------------

/**
 * @brief Compute the destination rectangle for a scale-to-fit presentation.
 *
 * The arithmetic is done in `double` so a 16:9 source fitted into a
 * 1000x1000 target rounds to an exact 1000x563 rather than an off-by-one.
 * The result is then rounded to the nearest integer.
 *
 * ## Degenerate inputs
 *
 * A zero (or negative) source or target dimension yields a **zero-sized**
 * rectangle — the function never divides by zero and never throws.  The
 * caller is expected to skip the present when the rectangle is empty.
 *
 * @param src_w  The source width in pixels.
 * @param src_h  The source height in pixels.
 * @param dst_w  The target width in pixels.
 * @param dst_h  The target height in pixels.
 * @param mode   The fit mode.
 * @return The destination rectangle.
 */
NIMRTC_API_STABLE inline ScaleRect compute_scale_to_fit(std::int32_t src_w,
                                                         std::int32_t src_h,
                                                         std::int32_t dst_w,
                                                         std::int32_t dst_h,
                                                         ScaleMode mode) noexcept {
    ScaleRect rect{};

    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        // Degenerate: nothing to draw.  Return an empty rectangle rather
        // than dividing by zero.
        return rect;
    }

    if (mode == ScaleMode::kStretch) {
        rect.x = 0;
        rect.y = 0;
        rect.w = dst_w;
        rect.h = dst_h;
        return rect;
    }

    const double src_aspect =
        static_cast<double>(src_w) / static_cast<double>(src_h);

    if (mode == ScaleMode::kCrop) {
        // Fill the target: the destination is exactly the target, and the
        // overflow on the long axis is cropped away.
        rect.x = 0;
        rect.y = 0;
        rect.w = dst_w;
        rect.h = dst_h;
        return rect;
    }

    // kLetterbox: fit the source inside the target.
    const double target_aspect =
        static_cast<double>(dst_w) / static_cast<double>(dst_h);

    double draw_w = 0.0;
    double draw_h = 0.0;
    if (src_aspect >= target_aspect) {
        // The source is wider (or equally wide): it fills the width and
        // the bars are on the top / bottom.
        draw_w = static_cast<double>(dst_w);
        draw_h = static_cast<double>(dst_w) / src_aspect;
    } else {
        // The source is taller: it fills the height and the bars are on
        // the left / right.
        draw_h = static_cast<double>(dst_h);
        draw_w = static_cast<double>(dst_h) * src_aspect;
    }

    rect.w = static_cast<std::int32_t>(draw_w + 0.5);
    rect.h = static_cast<std::int32_t>(draw_h + 0.5);
    // Never exceed the target (a rounding artefact must not spill out).
    if (rect.w > dst_w) rect.w = dst_w;
    if (rect.h > dst_h) rect.h = dst_h;

    rect.x = (dst_w - rect.w) / 2;
    rect.y = (dst_h - rect.h) / 2;
    return rect;
}

} // namespace nimrtc::video_sink
