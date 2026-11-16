/**
 * @file nimrtc/video_capture/cursor_source.hpp
 * @brief The platform cursor *source* seam — plan item **A6**.
 *
 * The compositor (`nimrtc/video_capture/cursor_compositor.hpp`) knows
 * *how* to blend a cursor into a frame; this header is the seam that
 * knows *where the cursor is and what it looks like*.
 *
 * Remote-desktop plan A6 names the per-platform sources:
 *
 *   - **Windows** — `GetCursorInfo` + `DrawIconEx` (the cursor handle
 *     is drawn into a DIB, which becomes the RGBA bitmap).
 *   - **X11 (Linux)** — `XFixesGetCursorImage`.
 *   - **macOS** — *nothing*: ScreenCaptureKit composites the pointer
 *     into the picture natively, so a cursor source reports "no
 *     overlay needed" and the capture device leaves the frame alone.
 *
 * ## The DXGI case (the reason this exists)
 *
 * DXGI Desktop Duplication does **not** composite the pointer into the
 * frame — that is the whole reason the cursor layer exists at all.
 * The WGC backend *does* composite it, so on the WGC path the cursor
 * layer is simply not engaged (`CaptureCapabilities::
 * cursor_composited` is `true` there and `false` for DXGI).
 *
 * ## The fallback
 *
 * A platform cursor can become unavailable at any moment: the pointer
 * left the captured display, the compositor is busy, the X11 server
 * hiccuped.  The plan asks for a `PinnedSink`-style fallback — "the
 * cursor is always drawn at the last position".  That is what
 * `PinnedCursorFallback` is: it wraps a source, remembers the last
 * successful sample, and serves *that* when the wrapped source
 * fails.
 *
 * @note Remote-desktop plan A6.
 */

#ifndef NIMRTC_VIDEO_CAPTURE_CURSOR_SOURCE_HPP
#define NIMRTC_VIDEO_CAPTURE_CURSOR_SOURCE_HPP

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <cstdint>
#include <memory>
#include <nimrtc/core/api_version.hpp>
#include <nimrtc/video_capture/cursor_compositor.hpp>
#include <string_view>
#include <vector>

namespace nimrtc::video_capture {

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

/** The outcome of asking a cursor source for the current pointer. */
enum class NIMRTC_API_STABLE CursorSourceStatus : std::uint8_t {
    /** A sample was produced. */
    kOk = 0,

    /** The platform composites the cursor itself, so **no overlay is
     *  needed** (macOS / ScreenCaptureKit).  This is not a failure:
     *  the caller simply does not composite anything. */
    kNoOverlayNeeded = 1,

    /** The cursor could not be read *right now* (the pointer left the
     *  captured surface, the compositor is busy, the platform call
     *  failed).  The caller may fall back to the last known sample. */
    kUnavailable = 2,

    /** This platform has no cursor source at all. */
    kUnsupported = 3,
};

/** Human-readable status name (for logs). */
NIMRTC_API_STABLE inline std::string_view cursor_source_status_name(
        CursorSourceStatus s) noexcept {
    switch (s) {
        case CursorSourceStatus::kOk:             return "ok";
        case CursorSourceStatus::kNoOverlayNeeded:return "no-overlay-needed";
        case CursorSourceStatus::kUnavailable:    return "unavailable";
        case CursorSourceStatus::kUnsupported:    return "unsupported";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// The sample
// ---------------------------------------------------------------------------

/**
 * @brief One reading of the pointer: its bitmap and where it is.
 */
struct NIMRTC_API_STABLE CursorSample {
    /** The cursor bitmap (RGBA8), with its hotspot. */
    CursorImage image;

    /** The hotspot's position in **screen** coordinates.
     *
     *  Screen coordinates, not frame coordinates: the source does not
     *  know where the captured surface sits on the desktop — only the
     *  capture device does.  Use `CursorCompositor::screen_to_frame()`
     *  to convert. */
    std::int32_t screen_x = 0;
    std::int32_t screen_y = 0;

    /** True iff the platform reported that the pointer is hidden
     *  (`CURSOR_SHOWING` on Windows).  A hidden pointer is drawn with
     *  zero alpha, so the compositor leaves the frame alone. */
    bool hidden = false;
};

// ---------------------------------------------------------------------------
// ICursorSource
// ---------------------------------------------------------------------------

/**
 * @brief A source of the pointer's bitmap and position.
 *
 * Implementations are **pluggable**: a capture device is handed one
 * through `set_cursor_source()`, so a host can substitute its own
 * (a remote cursor, a scripted pointer, a test double) without
 * touching the capture code.
 */
class NIMRTC_API_STABLE ICursorSource {
public:
    virtual ~ICursorSource() = default;

    /**
     * @brief Read the pointer's current state.
     *
     * @param out Receives the sample when the status is `kOk`.
     * @return The status.  `kNoOverlayNeeded` means the platform
     *         composites the pointer itself and no overlay should be
     *         drawn.
     */
    [[nodiscard]] virtual CursorSourceStatus poll(CursorSample& out) noexcept = 0;

    /** A short identifier for the source, e.g. "windows-cursor". */
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// PinnedCursorFallback
// ---------------------------------------------------------------------------

/**
 * @brief The "cursor is always drawn at the last position" fallback.
 *
 * Wraps a real cursor source and remembers the last sample it
 * produced.  When the wrapped source reports `kUnavailable`, the
 * fallback serves the **remembered** sample instead — so the pointer
 * stays visible at its last position instead of disappearing (which
 * looks like a flicker to the viewer) or the whole overlay being
 * dropped.
 *
 * The fallback is *not* a lie about the pointer: the remembered sample
 * carries the timestamp it was taken at, and the caller can compare it
 * against the current time to decide how stale the pin is.
 *
 * A "pin" only exists once the wrapped source has succeeded at least
 * once.  Before that, the fallback reports the wrapped source's status
 * verbatim — an honest "unavailable", not an empty sample.
 */
class NIMRTC_API_STABLE PinnedCursorFallback final : public ICursorSource {
public:
    /** Wrap @p inner.  A null @p inner is treated as a source that is
     *  always unavailable. */
    explicit PinnedCursorFallback(std::shared_ptr<ICursorSource> inner) noexcept;

    ~PinnedCursorFallback() override;

    // ---- ICursorSource ----------------------------------------------------
    [[nodiscard]] CursorSourceStatus poll(CursorSample& out) noexcept override;

    [[nodiscard]] std::string_view id() const noexcept override { return "pinned-cursor"; }

    // ---- Fallback-specific ------------------------------------------------

    /** The wrapped source. */
    [[nodiscard]] const std::shared_ptr<ICursorSource>& inner() const noexcept {
        return inner_;
    }

    /** True iff a sample has been pinned (the wrapped source has
     *  succeeded at least once). */
    [[nodiscard]] bool has_pinned_sample() const noexcept { return pinned_; }

    /**
     * @brief The pinned sample.
     *
     * @return The last successful sample, or null when nothing has
     *         been pinned yet.
     */
    [[nodiscard]] const CursorSample* pinned_sample() const noexcept {
        return pinned_ ? &pinned_sample_ : nullptr;
    }

    /** How many times the fallback served a pinned sample. */
    [[nodiscard]] std::uint64_t pinned_hits() const noexcept { return pinned_hits_; }

    /** Forget the pinned sample (e.g. when the capture region moves). */
    void clear_pin() noexcept;

private:
    std::shared_ptr<ICursorSource> inner_;
    CursorSample pinned_sample_{};
    bool pinned_ = false;
    std::uint64_t pinned_hits_ = 0;
};

// ---------------------------------------------------------------------------
// The platform sources
// ---------------------------------------------------------------------------

/**
 * @brief Create the platform's native cursor source.
 *
 *   - Windows: `GetCursorInfo` + `DrawIconEx` into a DIB.
 *   - X11:     `XFixesGetCursorImage`.
 *   - macOS:   a source that reports `kNoOverlayNeeded`, because
 *              ScreenCaptureKit composites the pointer natively.
 *   - anywhere else: a source that reports `kUnsupported`.
 *
 * @return The source, or null when the platform has none.
 */
NIMRTC_API_STABLE std::shared_ptr<ICursorSource> make_platform_cursor_source() noexcept;

/**
 * @brief Create the Windows cursor source (`GetCursorInfo` +
 *        `DrawIconEx`).
 *
 * Returns null on a non-Windows build.  Exposed separately so a test
 * can name the backend explicitly.
 */
NIMRTC_API_STABLE std::shared_ptr<ICursorSource> make_windows_cursor_source() noexcept;

/**
 * @brief Create the X11 cursor source (`XFixesGetCursorImage`).
 *
 * Returns null when the build has no X11 / XFixes support.
 */
NIMRTC_API_STABLE std::shared_ptr<ICursorSource> make_x11_cursor_source() noexcept;

/**
 * @brief Create the macOS cursor source.
 *
 * On macOS the source exists but always reports `kNoOverlayNeeded`:
 * ScreenCaptureKit composites the pointer into the picture itself, so
 * an overlay would draw the cursor *twice*.
 */
NIMRTC_API_STABLE std::shared_ptr<ICursorSource> make_macos_cursor_source() noexcept;

/** Whether this build has a native cursor source. */
NIMRTC_API_STABLE bool platform_cursor_source_available() noexcept;

} // namespace nimrtc::video_capture
#endif // NIMRTC_VIDEO_CAPTURE_CURSOR_SOURCE_HPP
