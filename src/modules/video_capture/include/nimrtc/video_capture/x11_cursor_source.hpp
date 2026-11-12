/**
 * @file nimrtc/video_capture/x11_cursor_source.hpp
 * @brief The X11 cursor source (remote-desktop plan A6).
 *
 * `XGetImage` does **not** include the pointer, so the X11 capture
 * backend needs the cursor layer to draw it.  This source is the Linux
 * half of that layer — the Windows half is `WinCursorSource`.
 *
 * ## How the pointer is read
 *
 * The pointer's *image* and *position* live in the **Xfixes** extension,
 * not in core X11:
 *
 *   - `XFixesGetCursorImage` returns the pointer's bitmap — the very
 *     same 32-bit BGRA shape the Windows cursor source produces, so
 *     the portable alpha compositor needs no conversion — together
 *     with its position and the screen it is on.
 *   - `XFixesGetActivePointer` is the cheaper "where is it" query, for
 *     when the bitmap has not changed.
 *
 * The extension is queried with `XFixesQueryExtension`; a server
 * without Xfixes (a very old one) simply has no pointer to report, and
 * the source says so rather than failing.
 *
 * ## Coordinate spaces
 *
 * `XFixesGetCursorImage` reports the pointer's position in *root
 * window* coordinates.  The captured frame is one screen's surface,
 * and a screen is not necessarily at the root window's origin (two
 * screens side by side have different origins).  `set_screen_origin`
 * translates, exactly like `WinCursorSource::set_display_origin` does
 * on Windows.
 *
 * ## Threading
 *
 * `query()` is safe to call from any thread, but the X11 connection is
 * not: the source opens **its own** connection to the display, and
 * serialises its queries with a mutex.  A cursor query is cheap and
 * non-blocking, so the contention is negligible.
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
 * @brief Reads the X11 pointer.
 *
 * On a non-Linux host the class exists (so a host can refer to it
 * portably) but always reports "no pointer".
 */
class NIMRTC_API_STABLE X11CursorSource final : public ICursorSource {
public:
    /**
     * @brief The default constructor.
     *
     * It is declared here but *defined* in the implementation file: the
     * pimpl's type is incomplete in this header, so an inline default
     * constructor would have to destroy a `unique_ptr` to an incomplete
     * type — which the standard forbids.
     */
    X11CursorSource() noexcept;
    ~X11CursorSource() override;

    X11CursorSource(const X11CursorSource&)            = delete;
    X11CursorSource& operator=(const X11CursorSource&) = delete;

    /** The pointer's current state. */
    CursorState query() noexcept override;

    /** `"x11-cursor"`. */
    [[nodiscard]] std::string_view backend() const noexcept override {
        return "x11-cursor";
    }

    /** True iff this host has the Xfixes extension and a display. */
    [[nodiscard]] bool has_live_cursor() const noexcept override;

    /**
     * @brief The captured screen's origin in the root window.
     *
     * @param x  The screen's left edge in root-window pixels.
     * @param y  The screen's top edge in root-window pixels.
     */
    void set_screen_origin(std::int32_t x, std::int32_t y) noexcept;

    /** The seam's spelling of the same thing: the captured surface's
     *  origin in the platform's global pointer space. */
    void set_surface_origin(std::int32_t x, std::int32_t y) noexcept override {
        set_screen_origin(x, y);
    }

private:
    /**
     * @brief The factory needs to install the pimpl.
     *
     * It is a friend so that the *public* factory function can build a
     * source without exposing the pimpl's type (which stays private to
     * the implementation file).
     */
    friend std::unique_ptr<ICursorSource> make_x11_cursor_source();

    mutable std::mutex mutex_;

    /** The X11 objects.  Opaque here so that the header stays free of
     *  the X11 SDK. */
    struct Impl;
    std::unique_ptr<Impl> impl_;

    /** The captured screen's origin in the root window. */
    std::int32_t origin_x_ = 0;
    std::int32_t origin_y_ = 0;
};

/**
 * @brief Create an X11 cursor source.
 *
 * Returns nullptr when the host has no X11 display or no Xfixes
 * extension — there is no pointer to read there, and a null source says
 * exactly that.
 */
[[nodiscard]] std::unique_ptr<ICursorSource> make_x11_cursor_source();

} // namespace nimrtc::video_capture
