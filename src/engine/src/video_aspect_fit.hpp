/**
 * @file src/engine/src/video_aspect_fit.hpp
 * @brief PRIVATE engine helper: the scale-to-fit geometry (plan E7).
 *
 * ## What this is
 *
 * Plan E7 asks for a *full-screen / scale-to-fit* presentation: the
 * decoded picture is fitted into the viewer's window **honouring the
 * aspect ratio**, so a 16:9 desktop shown in a 4:3 window is
 * letterboxed rather than stretched.
 *
 * The engine resolves the *request* (the target size plus the policy)
 * into the sink's configuration (`plugins::VideoSinkConfig`), which
 * carries the policy as `plugins::VideoScaleMode`.  The sink does the
 * actual scaling; the engine only has to say *which* policy applies
 * and *how large* the target is.
 *
 * ## Why the arithmetic is duplicated here
 *
 * The portable helper — `nimrtc::video_sink::compute_scale_to_fit()` in
 * `src/modules/video_sink/include/nimrtc/video_sink/scale_to_fit.hpp` —
 * is the module's own geometry, and it is unit-tested there.  The
 * engine **may not include it**: `video_sink` is a concrete module and
 * is not on the ADR-025 exemption list (Layout Invariant 4).  So the
 * small, closed-form arithmetic the engine needs is written out here
 * as an engine-private inline function.
 *
 * The two implementations agree by construction: both are pure
 * double-precision ratios with a round-to-nearest, and both clamp the
 * result to the target.  The engine's copy answers a *different*
 * question anyway — "what is the drawn size inside the target" — so
 * the sink's rectangle arithmetic is not needed here.
 *
 * This header is PRIVATE: it is not installed and no public header
 * includes it (Layout Invariant 4).
 *
 * @see docs/plan/remote-desktop-plan.md §Track E (E7 — scale-to-fit).
 */
#pragma once

#include <cstdint>

namespace nimrtc::engine::detail {

/**
 * @brief The drawn size of a source picture fitted into a target.
 *
 * The result is the size the picture occupies **inside** the target
 * rectangle, with the aspect ratio preserved.  For `kCrop` and
 * `kStretch` the drawn size is the target itself (the difference is
 * what happens to the overflow, and that is the sink's business).
 *
 * A degenerate dimension (zero or negative) yields a zero-sized
 * result — the function never divides by zero.
 *
 * @param src_w  The source width in pixels.
 * @param src_h  The source height in pixels.
 * @param dst_w  The target width in pixels.
 * @param dst_h  The target height in pixels.
 * @param letterbox  True = fit the whole picture inside the target
 *                   (letterbox); false = fill the target (crop).
 * @return The drawn width and height, in pixels.
 */
struct ScaleToFitResult {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

[[nodiscard]] inline ScaleToFitResult compute_scale_to_fit(
        std::uint32_t src_w,
        std::uint32_t src_h,
        std::uint32_t dst_w,
        std::uint32_t dst_h,
        bool letterbox) noexcept {
    ScaleToFitResult out{};

    if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) {
        return out;
    }

    if (!letterbox) {
        // kCrop / kStretch: the picture covers the whole target.
        out.width  = dst_w;
        out.height = dst_h;
        return out;
    }

    // kLetterbox: fit the source inside the target, keeping the ratio.
    //
    // The arithmetic is done in `double` so a 16:9 source fitted into a
    // 1000x1000 target rounds to an exact 1000x563 rather than an
    // off-by-one.
    const double src_aspect = static_cast<double>(src_w) / static_cast<double>(src_h);
    const double dst_aspect = static_cast<double>(dst_w) / static_cast<double>(dst_h);

    double draw_w = 0.0;
    double draw_h = 0.0;
    if (src_aspect >= dst_aspect) {
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

    auto w = static_cast<std::uint32_t>(draw_w + 0.5);
    auto h = static_cast<std::uint32_t>(draw_h + 0.5);
    if (w > dst_w) w = dst_w;
    if (h > dst_h) h = dst_h;

    out.width  = w;
    out.height = h;
    return out;
}

}  // namespace nimrtc::engine::detail
