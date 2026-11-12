// modules/video_capture/src/platform/win_cursor_source.cpp
// ----------------------------------------------------------------------------
// The Windows cursor source (remote-desktop plan A6).
//
// It *fetches* the pointer; it never draws it.  The drawing is the
// portable cursor layer's job, so that the same code path serves
// Windows, X11 and any future platform.
//
// See include/nimrtc/video_capture/win_cursor_source.hpp for the contract.
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/win_cursor_source.hpp>

#include <cstddef>
#include <utility>

#if defined(_WIN32)
#  define NIMRTC_WIN_CURSOR 1
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace nimrtc::video_capture {

#if defined(NIMRTC_WIN_CURSOR)

void WinCursorSource::set_display_origin(std::int32_t x,
                                         std::int32_t y) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    origin_x_ = x;
    origin_y_ = y;
}

namespace {

/**
 * @brief Read a GDI bitmap's pixels as 32-bit BGRA, top-down.
 *
 * Every bitmap GDI hands out for a cursor is read through this one
 * helper, and the helper *always* asks for 32 bits per pixel �?even for
 * the 1-bit AND/XOR masks.
 *
 * That is not a stylistic choice.  Asking `GetDIBits` to produce 1-bit
 * output for a cursor's mask bitmap makes GDI write past the end of the
 * caller's buffer (the mask's own `bmWidthBytes` stride and the
 * requested DIB stride disagree for a cursor mask), which corrupts the
 * heap.  Reading the same bitmap as 32 bits is well-defined: GDI
 * expands each mask bit to a full pixel, so the bit pattern is
 * recovered from the colour channels instead.
 *
 * @param bm    The bitmap to read.
 * @param w     Its width in pixels.
 * @param h     Its height in pixels.
 * @param dst   Receives `w * 4` bytes per row, top-down.
 *
 * @return true iff the whole bitmap was read.
 */
bool read_bgra(HBITMAP bm, int w, int h, std::vector<std::uint8_t>& dst,
                std::size_t* stride_out = nullptr) {
    if (bm == nullptr || w <= 0 || h <= 0) {
        return false;
    }

    const std::size_t stride =
        (static_cast<std::size_t>(w) * 4u + 3u) & ~std::size_t{3u};
    dst.assign(stride * static_cast<std::size_t>(h), 0u);
    if (stride_out != nullptr) {
        *stride_out = stride;
    }

    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = -h;   // negative: top-down
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    HDC dc = ::GetDC(nullptr);
    if (dc == nullptr) {
        return false;
    }
    const int lines =
        ::GetDIBits(dc, bm, 0, static_cast<UINT>(h), dst.data(),
                    reinterpret_cast<BITMAPINFO*>(&bih), DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, dc);

    return lines == static_cast<int>(h);
}

/**
 * @brief Read a mask bit out of a widened (32bpp) mask bitmap.
 *
 * GDI expands a 1-bit mask pixel to a full 32-bit pixel whose colour is
 * either all-zero (bit 0) or all-ones (bit 1), so the bit survives as
 * "is any colour channel non-zero".
 */
bool mask_bit(const std::vector<std::uint8_t>& mask, std::size_t stride,
              int x, int y) {
    const std::size_t off =
        static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4u;
    if (off + 3u >= mask.size()) {
        return false;
    }
    return (mask[off] | mask[off + 1u] | mask[off + 2u]) != 0;
}

} // namespace

void WinCursorSource::extract(void* cursor, Bitmap& out) noexcept {
    // `GetIconInfo` gives the colour bitmap, the AND mask and the
    // hotspot.  A legacy monochrome cursor has no colour bitmap at
    // all -- only the mask -- and its "colour" is white.
    ICONINFO info{};
    if (!::GetIconInfo(static_cast<HCURSOR>(cursor), &info)) {
        out = Bitmap{};
        return;
    }

    BITMAP mask_bm{};
    BITMAP color_bm{};
    const bool have_mask =
        info.hbmMask != nullptr &&
        ::GetObject(info.hbmMask, sizeof(mask_bm), &mask_bm) != 0;
    const bool have_color =
        info.hbmColor != nullptr &&
        ::GetObject(info.hbmColor, sizeof(color_bm), &color_bm) != 0;

    // Whatever happens below, the two bitmaps `GetIconInfo` handed us
    // are ours to delete.
    const auto release = [&info]() noexcept {
        if (info.hbmMask != nullptr) ::DeleteObject(info.hbmMask);
        if (info.hbmColor != nullptr) ::DeleteObject(info.hbmColor);
    };

    // The cursor's own geometry.  A monochrome cursor's mask is twice as
    // tall as the cursor (the AND plane sits above the XOR plane), so
    // the cursor's height is half the mask's.
    int width = 0;
    int height = 0;
    if (have_color && color_bm.bmWidth > 0 && color_bm.bmHeight > 0) {
        width = color_bm.bmWidth;
        height = color_bm.bmHeight;
    } else if (have_mask && mask_bm.bmWidth > 0 && mask_bm.bmHeight > 0) {
        width = mask_bm.bmWidth;
        height = mask_bm.bmHeight / 2;
    }
    if (width <= 0 || height <= 0) {
        release();
        out = Bitmap{};
        return;
    }

    const std::size_t out_stride =
        (static_cast<std::size_t>(width) * 4u + 3u) & ~std::size_t{3u};
    std::vector<std::uint8_t> pixels(out_stride * static_cast<std::size_t>(height), 0u);

    std::vector<std::uint8_t> color_bgra;
    std::vector<std::uint8_t> mask_bgra;
    std::size_t color_stride = 0;
    std::size_t mask_stride = 0;
    const bool color_ok =
        have_color && read_bgra(info.hbmColor, color_bm.bmWidth,
                                color_bm.bmHeight, color_bgra, &color_stride);

    if (color_ok) {
        // Does the colour bitmap carry a real alpha channel?  A modern
        // (32-bit) cursor does, and its alpha is authoritative: the
        // pointer's anti-aliased edges live there.
        bool alpha_ok = false;
        for (std::size_t i = 3; i < color_bgra.size(); i += 4) {
            if (color_bgra[i] != 0) {
                alpha_ok = true;
                break;
            }
        }

        if (alpha_ok) {
            // A modern cursor: the colour bitmap *is* the pointer.
            pixels = std::move(color_bgra);
        } else {
            // A 24-bit cursor: the colour bitmap has the pixels but no
            // alpha, and the AND mask says which of them are opaque.
            const int mask_h = mask_bm.bmHeight > 0 ? mask_bm.bmHeight : height;
            const bool mask_ok =
                have_mask && read_bgra(info.hbmMask, mask_bm.bmWidth, mask_h,
                                       mask_bgra, &mask_stride);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const std::size_t off =
                        static_cast<std::size_t>(y) * out_stride +
                        static_cast<std::size_t>(x) * 4u;
                    if (off + 3u >= color_bgra.size()) {
                        continue;
                    }
                    pixels[off + 0] = color_bgra[off + 0];
                    pixels[off + 1] = color_bgra[off + 1];
                    pixels[off + 2] = color_bgra[off + 2];
                    // In an AND mask a *set* bit means "transparent".
                    const bool transparent =
                        mask_ok && mask_bit(mask_bgra, mask_stride, x, y);
                    pixels[off + 3] = transparent ? 0x00u : 0xFFu;
                }
            }
        }
    } else {
        // A legacy monochrome cursor: the mask carries both planes.
        // Its top half is the AND mask (a set bit means "transparent")
        // and its bottom half is the XOR image (a set bit means
        // "white", a clear bit means "black").
        const bool mask_ok =
            have_mask && read_bgra(info.hbmMask, mask_bm.bmWidth,
                                   mask_bm.bmHeight, mask_bgra, &mask_stride);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t off =
                    static_cast<std::size_t>(y) * out_stride +
                    static_cast<std::size_t>(x) * 4u;
                if (!mask_ok) {
                    continue;
                }
                const bool transparent = mask_bit(mask_bgra, mask_stride, x, y);
                const bool white = mask_bit(mask_bgra, mask_stride, x, y + height);
                pixels[off + 0] = white ? 0xFFu : 0x00u;   // B
                pixels[off + 1] = white ? 0xFFu : 0x00u;   // G
                pixels[off + 2] = white ? 0xFFu : 0x00u;   // R
                pixels[off + 3] = transparent ? 0x00u : 0xFFu;
            }
        }
    }

    release();

    out.bgra = std::move(pixels);
    out.width = static_cast<std::uint32_t>(width);
    out.height = static_cast<std::uint32_t>(height);
    out.stride = static_cast<std::int32_t>(out_stride);
}

CursorState WinCursorSource::query() noexcept {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!::GetCursorInfo(&ci)) {
        return CursorState{};
    }

    // `CURSOR_SHOWING` is the flag that says the pointer is on screen.
    if ((ci.flags & CURSOR_SHOWING) == 0) {
        return CursorState{};
    }
    if (ci.hCursor == nullptr) {
        return CursorState{};
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Re-extract the bitmap only when the cursor *changed*.  The
        // position is re-read every time; the bitmap is not.
        if (cached_cursor_ != ci.hCursor) {
            Bitmap fresh;
            extract(ci.hCursor, fresh);
            cached_ = std::move(fresh);
            cached_cursor_ = ci.hCursor;
        }

        CursorState state;
        state.visible = true;
        state.live = true;
        state.x = static_cast<std::int32_t>(ci.ptScreenPos.x) - origin_x_;
        state.y = static_cast<std::int32_t>(ci.ptScreenPos.y) - origin_y_;
        state.bgra = cached_.bgra;
        state.bitmap_width = cached_.width;
        state.bitmap_height = cached_.height;
        state.bitmap_stride = cached_.stride;
        return state;
    }
}

#else // !NIMRTC_WIN_CURSOR

void WinCursorSource::set_display_origin(std::int32_t, std::int32_t) noexcept {}

void WinCursorSource::extract(void*, Bitmap&) noexcept {}

CursorState WinCursorSource::query() noexcept {
    // There is no pointer to read on a platform that has none.  The
    // class exists so a host can refer to it portably; it simply has
    // nothing to report.
    return CursorState{};
}

#endif // NIMRTC_WIN_CURSOR

std::unique_ptr<ICursorSource> make_win_cursor_source() {
#if defined(NIMRTC_WIN_CURSOR)
    return std::make_unique<WinCursorSource>();
#else
    return nullptr;
#endif
}

} // namespace nimrtc::video_capture
