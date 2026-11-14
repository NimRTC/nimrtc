// modules/video_capture/src/cursor_compositor.cpp
// ----------------------------------------------------------------------------
// The portable cursor-compositing layer (remote-desktop plan A6).
//
// The drawing itself is deliberately simple: an alpha-blended blit of a
// BGRA cursor bitmap into a BGRA frame.  The pointer bitmap arrives in
// the same layout the desktop compositor produces, so there is nothing
// platform-specific left in this file — which is the point: the
// platform backends (A3/A4/A5) only have to *fetch* the pointer, not
// draw it.
//
// See include/nimrtc/video_capture/cursor_compositor.hpp for the contract.
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/cursor_compositor.hpp>

#include <nimrtc/video_capture/win_cursor_source.hpp>
#include <nimrtc/video_capture/x11_cursor_source.hpp>

#include <algorithm>
#include <cstring>

namespace nimrtc::video_capture {

namespace {

/** Alpha-blend one BGRA texel over one BGRA frame texel. */
inline void blend_texel(std::uint8_t* dst,
                        std::uint8_t src_b,
                        std::uint8_t src_g,
                        std::uint8_t src_r,
                        std::uint8_t src_a) noexcept {
    if (src_a == 0) return;
    if (src_a == 255) {
        dst[0] = src_b;
        dst[1] = src_g;
        dst[2] = src_r;
        dst[3] = 0xFFu;
        return;
    }
    // Integer alpha blend: dst = src*a/255 + dst*(255-a)/255.
    const std::uint32_t a = src_a;
    const std::uint32_t inv = 255u - a;
    dst[0] = static_cast<std::uint8_t>((src_b * a + dst[0] * inv) / 255u);
    dst[1] = static_cast<std::uint8_t>((src_g * a + dst[1] * inv) / 255u);
    dst[2] = static_cast<std::uint8_t>((src_r * a + dst[2] * inv) / 255u);
    // The frame's alpha stays opaque: a captured desktop is opaque.
    dst[3] = 0xFFu;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// AlphaCursorCompositor
// ---------------------------------------------------------------------------

bool AlphaCursorCompositor::composite(std::uint8_t* frame,
                                       std::size_t frame_size,
                                       std::int32_t stride,
                                       std::uint32_t width,
                                       std::uint32_t height,
                                       const CursorState& state,
                                       std::int32_t origin_x,
                                       std::int32_t origin_y) noexcept {
    if (frame == nullptr || frame_size == 0) return false;
    if (width == 0 || height == 0) return false;
    if (stride < static_cast<std::int32_t>(width * 4u)) return false;

    // A hidden pointer is not a pointer to draw.
    if (!state.visible) return false;

    // A source with no bitmap can only report a position, and a
    // position with nothing to draw is nothing.
    if (state.bgra.empty() || state.bitmap_width == 0 || state.bitmap_height == 0) {
        return false;
    }

    const std::int32_t bm_stride = state.bitmap_stride > 0
        ? state.bitmap_stride
        : static_cast<std::int32_t>(state.bitmap_width * 4u);
    if (bm_stride < static_cast<std::int32_t>(state.bitmap_width * 4u)) {
        return false;
    }

    // The pointer's position is in *source* pixels; the frame may be a
    // sub-region of the source, so the origin is subtracted.
    const std::int32_t fx = state.x - origin_x;
    const std::int32_t fy = state.y - origin_y;

    // The bitmap's top-left is at the pointer's hotspot.  The hotspot
    // is conventionally the centre for the standard arrow, but the
    // source does not tell us where it is, so the bitmap is drawn with
    // its top-left at the pointer's position — the same convention
    // DXGI's own cursor drawing uses.
    const std::int32_t bx = fx;
    const std::int32_t by = fy;

    // Clip the bitmap against the frame.
    const std::int32_t x0 = std::max<std::int32_t>(bx, 0);
    const std::int32_t y0 = std::max<std::int32_t>(by, 0);
    const std::int32_t x1 = std::min<std::int32_t>(
        bx + static_cast<std::int32_t>(state.bitmap_width),
        static_cast<std::int32_t>(width));
    const std::int32_t y1 = std::min<std::int32_t>(
        by + static_cast<std::int32_t>(state.bitmap_height),
        static_cast<std::int32_t>(height));

    if (x0 >= x1 || y0 >= y1) {
        // The pointer is entirely outside the frame.  Nothing to do.
        return false;
    }

    for (std::int32_t y = y0; y < y1; ++y) {
        const std::uint8_t* src_row =
            state.bgra.data() + static_cast<std::ptrdiff_t>(y - by) * bm_stride;
        std::uint8_t* dst_row =
            frame + static_cast<std::ptrdiff_t>(y) * stride;

        for (std::int32_t x = x0; x < x1; ++x) {
            const std::uint8_t* s =
                src_row + static_cast<std::ptrdiff_t>(x - bx) * 4u;
            blend_texel(dst_row + static_cast<std::ptrdiff_t>(x) * 4u,
                        s[0], s[1], s[2], s[3]);
        }
    }

    ++frames_composited_;
    return true;
}

// ---------------------------------------------------------------------------
// PinnedCursorSource
// ---------------------------------------------------------------------------

CursorState PinnedCursorSource::query() noexcept {
    if (inner_ == nullptr) return CursorState{};

    const CursorState live = inner_->query();
    if (live.visible) {
        // Remember the last known position so it can be re-drawn if the
        // source goes quiet later.
        pinned_ = live;
        return live;
    }

    // The source could not report a live pointer.  Fall back to the
    // pinned one — but only if the inner source *can* report a live
    // pointer at all.  A source that never has one (the null source)
    // must not produce a phantom pointer.
    if (!inner_->has_live_cursor()) return CursorState{};

    pinned_.visible = true;
    pinned_.live = false;
    return pinned_;
}

// ---------------------------------------------------------------------------
// The platform cursor source
// ---------------------------------------------------------------------------

std::unique_ptr<ICursorSource> make_platform_cursor_source() {
    // The pointer is read differently on each platform, but the *shape*
    // of the answer is the same: a BGRA bitmap plus a position.  This
    // factory is the single place that knows which platform it is on,
    // so the adapter above it stays portable.
    //
    // The order matters only in that the platform-specific factory is
    // asked first: on macOS neither Windows nor X11 is meaningful, and
    // both return nullptr there anyway.
    if (auto win = make_win_cursor_source()) {
        return win;
    }
    if (auto x11 = make_x11_cursor_source()) {
        return x11;
    }

    // macOS (ScreenCaptureKit composites the pointer itself), or a
    // build without the pointer libraries.  There is no pointer to
    // read, so there is no source — and the caller treats a null
    // source as "leave the frame alone".
    return nullptr;
}

} // namespace nimrtc::video_capture
