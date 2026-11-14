// src/modules/video_frame/src/roi.cpp
// ----------------------------------------------------------------------------
// Scaling a source-space rectangle into an output resolution (plan C4).
// ----------------------------------------------------------------------------

#include "nimrtc/video_frame/roi.hpp"

#include <cmath>

namespace nimrtc::video_frame {
namespace {

/// Scale one coordinate, rounding to the nearest integer.
std::uint32_t scale_coord(std::uint32_t value,
                          std::uint32_t src_extent,
                          std::uint32_t dst_extent) noexcept {
    if (src_extent == 0 || dst_extent == 0) {
        return 0;
    }
    const double scaled = static_cast<double>(value) *
                          static_cast<double>(dst_extent) /
                          static_cast<double>(src_extent);
    return static_cast<std::uint32_t>(std::lround(scaled));
}

} // namespace

std::optional<ScaledRect> scale_region(std::uint32_t src_x,
                                       std::uint32_t src_y,
                                       std::uint32_t src_width,
                                       std::uint32_t src_height,
                                       std::uint32_t capture_w,
                                       std::uint32_t capture_h,
                                       std::uint32_t out_w,
                                       std::uint32_t out_h,
                                       std::uint32_t min_size) noexcept {
    if (out_w == 0 || out_h == 0) return std::nullopt;
    if (capture_w == 0 || capture_h == 0) return std::nullopt;
    if (src_width == 0 || src_height == 0) return std::nullopt;

    ScaledRect out;
    out.x      = scale_coord(src_x, capture_w, out_w);
    out.y      = scale_coord(src_y, capture_h, out_h);
    out.width  = scale_coord(src_width, capture_w, out_w);
    out.height = scale_coord(src_height, capture_h, out_h);

    // Clamp to the output frame.  The clamp is what keeps a rectangle
    // that hangs off the right edge of the capture from producing one
    // that hangs off the right edge of the output.
    if (out.x >= out_w) return std::nullopt;
    if (out.y >= out_h) return std::nullopt;
    if (out.x + out.width > out_w) out.width = out_w - out.x;
    if (out.y + out.height > out_h) out.height = out_h - out.y;

    if (out.empty()) return std::nullopt;
    if (min_size != 0 && (out.width < min_size || out.height < min_size)) {
        return std::nullopt;
    }
    return out;
}

} // namespace nimrtc::video_frame
