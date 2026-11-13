/**
 * @file nimrtc/video_capture/win_cursor_source.hpp
 * @brief The Windows cursor source (remote-desktop plan A6).
 *
 * DXGI Desktop Duplication hands over the *raw* desktop texture — the
 * pointer is not in it.  This source fetches the pointer so the
 * portable cursor layer can draw it.
 *
 * ## How the pointer is read
 *
 * `GetCursorInfo` reports the current cursor's handle and its
 * **hotspot position** in *virtual-desktop* screen coordinates.  The
 * bitmap itself comes from `GetIconInfo`, which yields the colour
 * bitmap, the 1-bit AND mask and the hotspot offset.
 *
 * Two shapes exist:
 *
 *   - **Modern cursors** (Windows 10 and later) are 32-bit BGRA with
 *     a real alpha channel.  `GetDIBits` copies the DIB section's bits
 *     verbatim, so the alpha survives and the pointer's
 *     anti-aliased edges blend correctly.
 *   - **Legacy monochrome cursors** have no colour bitmap at all —
 *     only a 1-bit AND/XOR mask.  For those the alpha is derived from
 *     the AND mask (a set bit means "transparent") and the colour is
 *     white.
 *
 * The source reads the alpha from the colour bitmap when it carries
 * one and falls back to the AND mask when it does not, so both shapes
 * work without a special case at the call site.
 *
 * ## Coordinate spaces
 *
 * `GetCursorInfo` reports the hotspot in *virtual-desktop* screen
 * coordinates, but a captured frame is one **display's** surface, and
 * a display is not at the virtual desktop's origin.  `set_display_origin`
 * translates: the source reports the pointer's position in the
 * captured display's own coordinate space, which is what the
 * compositor expects (the adapter then rebases it onto the selected
 * region's origin).
 *
 * ## Threading
 *
 * `query()` is safe to call from any thread.  The bitmap extraction is
 * cached by cursor handle, so a pointer that does not change is not
 * re-extracted every frame — only the position is re-read.
 *
 * @note Remote-desktop plan A6.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>

#include <nimrtc/video_capture/cursor_compositor.hpp>

namespace nimrtc::video_capture {

/**
 * @brief Reads the Windows pointer.
 *
 * On a non-Windows host the class exists (so a host can refer to it
 * portably) but always reports "no pointer".
 */
class NIMRTC_API_STABLE WinCursorSource final : public ICursorSource {
public:
    WinCursorSource() noexcept = default;
    ~WinCursorSource() override = default;

    WinCursorSource(const WinCursorSource&)            = delete;
    WinCursorSource& operator=(const WinCursorSource&) = delete;

    /** The pointer's current state. */
    CursorState query() noexcept override;

    /** `"win-cursor"`. */
    [[nodiscard]] std::string_view backend() const noexcept override {
        return "win-cursor";
    }

    /** True — Windows always has a pointer to report. */
    [[nodiscard]] bool has_live_cursor() const noexcept override {
        return true;
    }

    /**
     * @brief The captured display's origin in the virtual desktop.
     *
     * `GetCursorInfo` reports the hotspot in virtual-desktop screen
     * coordinates; the captured frame is the *display's* surface, so
     * the display's own origin has to be subtracted.
     *
     * @param x  The display's left edge in virtual-desktop pixels.
     * @param y  The display's top edge in virtual-desktop pixels.
     */
    void set_display_origin(std::int32_t x, std::int32_t y) noexcept;

    /** The seam's spelling of the same thing: the captured surface's
     *  origin in the platform's global pointer space. */
    void set_surface_origin(std::int32_t x, std::int32_t y) noexcept override {
        set_display_origin(x, y);
    }

private:
    /** The cached bitmap, keyed by the cursor handle it came from. */
    struct Bitmap {
        std::vector<std::uint8_t> bgra;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::int32_t stride = 0;
    };

    /** Extract the bitmap for @p cursor (a `HCURSOR`). */
    void extract(void* cursor, Bitmap& out) noexcept;

    std::mutex mutex_;

    /** The cursor handle the cached bitmap came from (nullptr: none). */
    void* cached_cursor_ = nullptr;

    /** The cached bitmap. */
    Bitmap cached_{};

    /** The captured display's origin in the virtual desktop. */
    std::int32_t origin_x_ = 0;
    std::int32_t origin_y_ = 0;
};

/**
 * @brief Create a Windows cursor source.
 *
 * Returns nullptr on a non-Windows host — there is no pointer to read
 * there, and a null source says exactly that.
 */
[[nodiscard]] std::unique_ptr<ICursorSource> make_win_cursor_source();

} // namespace nimrtc::video_capture
