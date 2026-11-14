// modules/video_capture/src/platform/windows/windows_cursor.cpp
// ----------------------------------------------------------------------------
// The Windows cursor source (remote-desktop plan A6).
//
// DXGI Desktop Duplication does **not** composite the pointer into the
// frame it delivers — the pointer is a separate hardware overlay — so
// the Windows capture path needs a cursor layer of its own.
//
// This adapter is the Windows half of that layer:
//
//   1. `GetCursorInfo` gives the pointer's screen position and the
//      cursor's `HCURSOR` handle.
//   2. `GetIconInfo` gives the hotspot (the point inside the bitmap that
//      corresponds to the pointer's position) and the icon's bitmaps.
//   3. `DrawIconEx` renders the icon into a 32-bit top-down DIB, which
//      is the BGRA layout the compositor blends.
//
// The result is a `CursorImage` — the same shape the X11 source
// produces — so the portable compositor does not know or care which
// platform it is running on.
//
// ## The alpha channel
//
// `DrawIconEx` does not reliably write an alpha channel into the
// destination DIB, so the alpha is reconstructed from the icon's
// monochrome mask (`GetIconInfo`'s `hbmMask`), which follows the
// classic convention: **a set bit means "transparent"**.  Where the
// drawn pixels already carry a non-zero alpha, that alpha is kept —
// a modern 32-bit cursor therefore keeps its soft edges.
//
// ## The coordinates
//
// `GetCursorInfo` reports the pointer's position in **screen**
// coordinates.  The compositor needs it in **frame** coordinates, so a
// capture backend that captures one display of a multi-display desktop
// must translate it (subtract the display's origin) before handing it
// over.  The source deliberately does *not* guess: a wrong guess would
// draw the pointer in the wrong place, which is worse than not drawing
// it at all.
//
// @note Remote-desktop plan A6.  Platform adapter — the portable half
//       lives in `src/cursor_compositor.cpp`.
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/cursor_source.hpp>

#include <cstring>
#include <vector>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cmath>

namespace nimrtc::video_capture {

namespace {

/**
 * @brief The Windows cursor source.
 *
 * It keeps the last rendered bitmap around so that a cursor that does
 * not change (the common case — the pointer moves, the *shape* does
 * not) does not have to be re-rendered on every frame.  Only the
 * position changes, and that is free.
 */
class WindowsCursorSource final : public ICursorSource {
  public:
    WindowsCursorSource() = default;

    ~WindowsCursorSource() override = default;

    [[nodiscard]] std::string_view id() const noexcept override { return "windows-cursor"; }

    [[nodiscard]] CursorSourceStatus poll(CursorSample& out) noexcept override {
        CURSORINFO info{};
        info.cbSize = sizeof(info);
        if (!::GetCursorInfo(&info)) {
            // The pointer is not available (a locked workstation, a
            // secure desktop, or a session that has no console).  This
            // is *not* an error: it is what drives the pinned fallback.
            return CursorSourceStatus::kUnavailable;
        }

        out.screen_x = info.ptScreenPos.x;
        out.screen_y = info.ptScreenPos.y;

        // The pointer is hidden: there is nothing to draw, and the
        // pinned fallback should not be updated with a stale shape.
        if ((info.flags & CURSOR_SHOWING) == 0) {
            out.hidden = true;
            out.image = CursorImage{};
            return CursorSourceStatus::kOk;
        }
        out.hidden = false;

        // The shape only has to be re-rendered when the cursor handle
        // changes.  The *position* changes all the time and costs
        // nothing.
        if (info.hCursor != last_handle_ || !cached_.valid()) {
            if (!render(info.hCursor, cached_)) {
                return CursorSourceStatus::kUnavailable;
            }
            last_handle_ = info.hCursor;
        }

        out.image = cached_;
        return CursorSourceStatus::kOk;
    }

  private:
    /**
     * @brief Render an `HCURSOR` into a 32-bit BGRA image.
     *
     * @param cursor The cursor handle.
     * @param out    Receives the image (bitmap + hotspot).
     */
    static bool render(HICON cursor, CursorImage& out) noexcept {
        if (cursor == nullptr) return false;

        ICONINFO icon{};
        if (!::GetIconInfo(cursor, &icon)) return false;

        // ---- The geometry ------------------------------------------------
        //
        // The cursor's size comes from the icon's *colour* bitmap when
        // there is one; otherwise it is derived from the system metrics
        // (the classic AND/XOR mask pair is half the size in each
        // dimension).
        int width = 0;
        int height = 0;
        if (icon.hbmColor != nullptr) {
            BITMAP bm{};
            if (::GetObject(icon.hbmColor, sizeof(bm), &bm) == 0) return false;
            width = bm.bmWidth;
            height = bm.bmHeight < 0 ? -bm.bmHeight : bm.bmHeight;
        } else {
            width = ::GetSystemMetrics(SM_CXCURSOR);
            height = ::GetSystemMetrics(SM_CYCURSOR);
        }
        if (width <= 0 || height <= 0) return false;

        // ---- The DIB -----------------------------------------------------
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;  // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HDC screen = ::GetDC(nullptr);
        if (screen == nullptr) return false;

        HBITMAP dib = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        ::ReleaseDC(nullptr, screen);
        if (dib == nullptr || bits == nullptr) {
            if (dib != nullptr) ::DeleteObject(dib);
            return false;
        }

        HDC mem = ::CreateCompatibleDC(nullptr);
        if (mem == nullptr) {
            ::DeleteObject(dib);
            return false;
        }
        HGDIOBJ old = ::SelectObject(mem, dib);

        // The icon is drawn at the origin of the DIB.  `DI_NORMAL`
        // keeps the mask's transparency; the alpha is reconstructed
        // below.
        const BOOL drawn =
            ::DrawIconEx(mem, 0, 0, cursor, width, height, 0, nullptr, DI_NORMAL);
        ::SelectObject(mem, old);
        ::DeleteDC(mem);

        if (!drawn) {
            ::DeleteObject(dib);
            return false;
        }

        // ---- The pixels --------------------------------------------------
        const std::size_t stride = static_cast<std::size_t>(width) * 4u;
        std::vector<std::uint8_t> pixels(stride * static_cast<std::size_t>(height));
        std::memcpy(pixels.data(), bits, pixels.size());

        // ---- The alpha ---------------------------------------------------
        //
        // `DrawIconEx` does not reliably write an alpha channel, so the
        // transparency is reconstructed from the icon's monochrome mask.
        // The mask's convention is "a set bit means transparent".
        bool any_drawn_alpha = false;
        for (std::size_t i = 3; i < pixels.size(); i += 4) {
            if (pixels[i] != 0) {
                any_drawn_alpha = true;
                break;
            }
        }

        apply_mask(icon.hbmMask, pixels, width, height, any_drawn_alpha);

        ::DeleteObject(dib);

        // ---- The image ----------------------------------------------------
        out.width = static_cast<std::uint32_t>(width);
        out.height = static_cast<std::uint32_t>(height);
        out.hotspot_x = icon.xHotspot;
        out.hotspot_y = icon.yHotspot;
        out.stride = static_cast<std::int32_t>(stride);
        out.pixels = std::move(pixels);
        return out.valid();
    }

    /**
     * @brief Apply the icon's monochrome mask to the drawn pixels.
     *
     * @param hbm_mask        The mask bitmap (may be null).
     * @param pixels          The drawn BGRA pixels.
     * @param width           The icon's width in pixels.
     * @param height          The icon's height in pixels.
     * @param any_drawn_alpha Whether the drawn pixels already carried a
     *                        non-zero alpha.
     */
    static void apply_mask(HBITMAP hbm_mask, std::vector<std::uint8_t>& pixels, int width,
                           int height, bool any_drawn_alpha) noexcept {
        auto make_opaque = [&]() {
            if (!any_drawn_alpha) {
                for (std::size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 0xFF;
            }
        };

        if (hbm_mask == nullptr) {
            make_opaque();
            return;
        }

        HDC screen = ::GetDC(nullptr);
        if (screen == nullptr) {
            make_opaque();
            return;
        }
        HDC mem = ::CreateCompatibleDC(screen);
        if (mem == nullptr) {
            ::ReleaseDC(nullptr, screen);
            make_opaque();
            return;
        }

        BITMAP bm{};
        if (::GetObject(hbm_mask, sizeof(bm), &bm) == 0) {
            ::DeleteDC(mem);
            ::ReleaseDC(nullptr, screen);
            make_opaque();
            return;
        }
        HGDIOBJ old = ::SelectObject(mem, hbm_mask);

        const int mask_height = bm.bmHeight < 0 ? -bm.bmHeight : bm.bmHeight;
        const int mask_width = bm.bmWidth;
        if (mask_width != width || mask_height != height) {
            // The mask does not match the icon's dimensions (a legacy
            // AND/XOR pair, for instance).  It cannot be applied
            // pixel-for-pixel, so the drawn alpha is left as it is.
            ::SelectObject(mem, old);
            ::DeleteDC(mem);
            ::ReleaseDC(nullptr, screen);
            make_opaque();
            return;
        }

        // A monochrome mask is 1 bit per pixel.  `GetDIBits` returns
        // 1-bit rows padded to a 4-byte boundary.
        const std::size_t row_bytes = ((static_cast<std::size_t>(width) + 31u) / 32u) * 4u;
        std::vector<std::uint8_t> row(row_bytes);

        for (int y = 0; y < height; ++y) {
            // `bm.bmHeight` is positive for a bottom-up DIB, so row 0 of
            // the icon is the *last* row of the mask.
            const int mask_row = (bm.bmHeight > 0) ? (height - 1 - y) : y;
            const int copied = ::GetDIBits(mem, hbm_mask, static_cast<UINT>(mask_row), 1,
                                           row.data(), nullptr, DIB_RGB_COLORS);
            if (copied != 1) continue;

            for (int x = 0; x < width; ++x) {
                const std::size_t byte_index = static_cast<std::size_t>(x) / 8u;
                if (byte_index >= row_bytes) continue;
                const std::uint8_t byte = row[byte_index];
                const std::uint8_t bit = (byte >> (x % 8)) & 0x01u;
                const std::size_t pixel_index =
                    (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(x)) * 4u;
                if (bit != 0) {
                    // Masked out: fully transparent.
                    pixels[pixel_index + 3] = 0;
                } else if (!any_drawn_alpha) {
                    pixels[pixel_index + 3] = 0xFF;
                }
            }
        }

        ::SelectObject(mem, old);
        ::DeleteDC(mem);
        ::ReleaseDC(nullptr, screen);
    }

    /** The last rendered cursor's bitmap. */
    CursorImage cached_{};
    /** The `HCURSOR` the cached bitmap was rendered from. */
    HCURSOR last_handle_ = nullptr;
};

} // anonymous namespace

std::shared_ptr<ICursorSource> make_windows_cursor_source() noexcept {
    return std::make_shared<WindowsCursorSource>();
}

} // namespace nimrtc::video_capture

#endif // defined(_WIN32)
