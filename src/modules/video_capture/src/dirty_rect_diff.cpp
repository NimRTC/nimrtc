// modules/video_capture/src/dirty_rect_diff.cpp
// ----------------------------------------------------------------------------
// The portable dirty-rectangle detector (remote-desktop plan A7).
//
// See include/nimrtc/video_capture/dirty_rect_diff.hpp for the rationale.
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/dirty_rect_diff.hpp>

#include <algorithm>
#include <cstring>

namespace nimrtc::video_capture {

void DirtyRectDetector::reset() noexcept {
    previous_.clear();
    previous_.shrink_to_fit();
    prev_width_  = 0;
    prev_height_ = 0;
    prev_stride_ = 0;
    prev_bpp_    = 0;
    has_previous_ = false;
}

void DirtyRectDetector::remember(const std::uint8_t* pixels,
                                 std::int32_t row_stride,
                                 std::size_t row_bytes,
                                 std::uint32_t width,
                                 std::uint32_t height,
                                 std::uint32_t bytes_per_pixel) noexcept {
    const std::size_t needed = row_bytes * static_cast<std::size_t>(height);
    if (previous_.size() != needed) {
        previous_.assign(needed, 0u);
    }
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(previous_.data() +
                        static_cast<std::size_t>(row) * row_bytes,
                    pixels + static_cast<std::size_t>(row) *
                        static_cast<std::size_t>(row_stride),
                    row_bytes);
    }
    prev_width_   = width;
    prev_height_  = height;
    prev_stride_  = row_stride;
    prev_bpp_     = bytes_per_pixel;
    has_previous_ = true;
}

bool DirtyRectDetector::compare(const std::uint8_t* pixels,
                                std::size_t size,
                                std::int32_t stride,
                                std::uint32_t width,
                                std::uint32_t height,
                                std::uint32_t bytes_per_pixel,
                                plugins::CaptureDirtyRects& out) noexcept {
    out.rects.clear();
    out.full_frame = false;

    if (pixels == nullptr || size == 0 || width == 0 || height == 0) {
        // Nothing to compare — the caller has to treat the frame as
        // fully changed rather than as "no change".
        out.full_frame = true;
        return false;
    }
    if (bytes_per_pixel == 0) bytes_per_pixel = 4;

    const std::size_t row_bytes =
        static_cast<std::size_t>(width) * bytes_per_pixel;
    if (row_bytes == 0) {
        out.full_frame = true;
        return false;
    }

    // The stride is the distance between two rows; when the caller does
    // not know it, the rows are packed.
    const std::int32_t row_stride =
        stride > 0 ? stride : static_cast<std::int32_t>(row_bytes);

    // ---- The geometry changed: everything is new ------------------------
    //
    // Comparing a 1920x1080 frame against a 1280x720 one is
    // meaningless, so the change is reported as a full frame.
    const bool geometry_changed =
        has_previous_ &&
        (prev_width_ != width || prev_height_ != height ||
         prev_stride_ != row_stride || prev_bpp_ != bytes_per_pixel);

    // ---- The first frame: everything is new ----------------------------
    //
    // There is no previous frame to compare against, so the frame is
    // reported as a full change.  It is *not* reported as "identical"
    // — the return value says "the frame carries no new information",
    // and the first frame certainly carries information.
    if (!has_previous_ || geometry_changed) {
        remember(pixels, row_stride, row_bytes, width, height,
                 bytes_per_pixel);
        out.full_frame = true;
        return false;
    }

    // ---- Compare ---------------------------------------------------------
    //
    // The scan walks the frame in *blocks* of `stride_` pixels and
    // compares one pixel per block.  The first and the last changed
    // block in each direction bound the changed region.
    //
    // A block that differs marks the whole block as changed: the
    // reported rectangle therefore never *under*-covers a change, which
    // is the safe direction (a too-large rectangle costs a little
    // encoding; a too-small one would show corruption).
    std::uint32_t min_x = width;
    std::uint32_t min_y = height;
    std::uint32_t max_x = 0;
    std::uint32_t max_y = 0;
    bool changed = false;

    const std::uint32_t step = stride_;
    for (std::uint32_t y = 0; y < height; y += step) {
        const std::uint8_t* new_row = pixels +
            static_cast<std::size_t>(y) * row_stride;
        const std::uint8_t* old_row = previous_.data() +
            static_cast<std::size_t>(y) * row_bytes;

        bool row_changed = false;
        for (std::uint32_t x = 0; x < width; x += step) {
            const std::size_t off = static_cast<std::size_t>(x) *
                                    bytes_per_pixel;
            if (std::memcmp(new_row + off, old_row + off, bytes_per_pixel) != 0) {
                row_changed = true;
                changed = true;
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                break;   // the row's right edge is found below
            }
        }
        if (row_changed) {
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
            // The row's *right* edge has to be found: the loop above
            // stopped at the first changed pixel.
            for (std::uint32_t x = width; x-- > 0;) {
                if (x % step != 0) continue;
                const std::size_t off = static_cast<std::size_t>(x) *
                                        bytes_per_pixel;
                if (std::memcmp(new_row + off, old_row + off, bytes_per_pixel) != 0) {
                    if (x > max_x) max_x = x;
                    break;
                }
            }
        }
    }

    // ---- Remember this frame -------------------------------------------
    //
    // The copy happens *after* the comparison, so the comparison sees
    // the previous frame.  The copy is row by row into a *tightly
    // packed* buffer, because the caller's stride may be wider than the
    // picture (the queue's own padding).  The packed layout is what the
    // comparison reads, so the two must agree on the stride:
    // `previous_`'s row stride is `row_bytes`, not `row_stride`.
    remember(pixels, row_stride, row_bytes, width, height, bytes_per_pixel);

    if (!changed) {
        // Nothing changed: the consumer can skip the frame entirely.
        return true;
    }

    // ---- Expand to the block boundaries ---------------------------------
    //
    // The sample at `x` represents the block `[x, x + step)`, so the
    // changed region is expanded to the right and downward.  Clamping
    // to the frame's edge keeps the rectangle inside the picture.
    const std::uint32_t x0 = (min_x / step) * step;
    const std::uint32_t y0 = (min_y / step) * step;
    const std::uint32_t x1 = std::min(width,  ((max_x / step) + 1u) * step);
    const std::uint32_t y1 = std::min(height, ((max_y / step) + 1u) * step);

    plugins::CaptureRegion r;
    r.x = x0;
    r.y = y0;
    r.width  = (x1 > x0) ? (x1 - x0) : 1u;
    r.height = (y1 > y0) ? (y1 - y0) : 1u;
    out.rects.push_back(r);
    return false;
}

} // namespace nimrtc::video_capture
