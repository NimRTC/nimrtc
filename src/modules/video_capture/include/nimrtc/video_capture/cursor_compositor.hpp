/**
 * @file nimrtc/video_capture/cursor_compositor.hpp
 * @brief A pluggable cursor-compositing layer (remote-desktop plan A6).
 *
 * Some capture backends composite the pointer into the frame themselves
 * and some do not:
 *
 *   - **Windows Graphics Capture** composites the cursor — the OS draws
 *     it into the captured surface.
 *   - **macOS ScreenCaptureKit** composites the cursor — the OS draws it
 *     into the captured surface.
 *   - **DXGI Desktop Duplication** does **not** — it hands over the raw
 *     desktop texture with no pointer in it.
 *   - **X11's `XGetImage`** does not, and neither does a raw PipeWire
 *     stream unless the portal asked for the cursor metadata.
 *
 * So the *cursor* is a separate, pluggable concern: a compositor takes a
 * frame and draws a pointer into it.  The plan's A6 row asks for exactly
 * this shape:
 *
 * @code
 *   ICursorSource   --where is the pointer, and what does it look like?
 *        │
 *        ▼
 *   ICursorCompositor --draw it into the frame
 *        │
 *        ▼
 *   the captured BGRA frame
 * @endcode
 *
 * ## Why a separate seam rather than a flag
 *
 * A flag (`want_cursor = true`) cannot express "the platform already
 * did it" versus "please draw it" versus "the platform has no pointer
 * at all".  The seam does:
 *
 *   - `ICursorSource` reports whether a pointer is *available* right now
 *     (a cursor that is hidden is not a cursor to draw),
 *   - `ICursorCompositor` decides whether it *should* draw (the backend
 *     already composited it → don't double-draw),
 *   - and a **pinned** fallback keeps the pointer visible at its last
 *     known position when the platform cannot report a live one, which
 *     is the plan's stated "PinnedSink-style fallback".
 *
 * ## The drawing is alpha-blended, not opaque
 *
 * The pointer is drawn with alpha blending, so a 32-bit cursor's
 * anti-aliased edges land correctly on top of the desktop pixels.  A
 * cursor bitmap is BGRA (the same layout the desktop compositors
 * produce), so the compositing path needs no conversion.
 *
 * ## Threading
 *
 * The compositor is called from the engine's tick thread (the same
 * thread that pops the capture frame), so it needs no internal
 * locking.  The *source* may be read from another thread (the platform
 * cursor query is cheap and non-blocking), so the source implementations
 * are responsible for their own synchronisation.
 *
 * @note Remote-desktop plan A6.
 */

#pragma once

// Stability classification: NIMRTC_API_STABLE (visual marker;
// see ADR-015 / docs/plan/v0.13-plan.md).
#include <nimrtc/core/api_version.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace nimrtc::video_capture {

// ---------------------------------------------------------------------------
// The cursor state
// ---------------------------------------------------------------------------

/** Where the pointer is and what it looks like. */
struct NIMRTC_API_STABLE CursorState {
    /** True iff a pointer is visible right now.  A hidden pointer is not
     *  a pointer to draw — the compositor leaves the frame alone. */
    bool visible = false;

    /** The pointer's position, in *source* pixels of the captured
     *  surface.  The compositor maps it into the frame's own
     *  coordinate space, which matters when a region was selected. */
    std::int32_t x = 0;
    std::int32_t y = 0;

    /** The pointer's bitmap, as 32-bit BGRA (the layout every desktop
     *  compositor produces).  Empty when the source has no bitmap to
     *  offer (a "position only" source). */
    std::vector<std::uint8_t> bgra;

    /** The bitmap's width in pixels. */
    std::uint32_t bitmap_width = 0;

    /** The bitmap's height in pixels. */
    std::uint32_t bitmap_height = 0;

    /** The bitmap's row stride in bytes (>= bitmap_width * 4). */
    std::int32_t bitmap_stride = 0;

    /** True iff the source could report a *live* pointer.  A false here
     *  with `visible == true` means the pointer is **pinned**: the
     *  compositor must keep drawing it where it was. */
    bool live = false;
};

// ---------------------------------------------------------------------------
// ICursorSource
// ---------------------------------------------------------------------------

/**
 * @brief Where the pointer is.
 *
 * A source is a *reader*, not a writer: it reports the pointer's state
 * and never touches the frame.  The platform implementations are:
 *
 *   - Windows: `GetCursorInfo` + `CopyIcon` / `GetIconInfo`
 *   - X11: `XFixesGetCursorImage`
 *   - macOS: ScreenCaptureKit already composites, so there is no source
 *   - the portable fallback: a source that always reports "no pointer"
 *
 * Thread safety: the query methods must be safe to call from any
 * thread (the platform cursor query is a cheap, non-blocking read).
 */
class NIMRTC_API_STABLE ICursorSource {
public:
    virtual ~ICursorSource() = default;

    /** Read the pointer's current state. */
    virtual CursorState query() noexcept = 0;

    /** A short identifier for the source, e.g. "win-cursor", "x11-cursor",
     *  "none". */
    [[nodiscard]] virtual std::string_view backend() const noexcept = 0;

    /** True iff this source can report a live pointer at all.  A false
     *  here means the platform has no pointer to report, so the
     *  compositor's pinned fallback is the only option. */
    [[nodiscard]] virtual bool has_live_cursor() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// ICursorCompositor
// ---------------------------------------------------------------------------

/**
 * @brief Draws a pointer into a captured frame.
 *
 * The compositor owns the *decision* of whether to draw; the source
 * owns the *state* of the pointer.  A backend that composites the
 * cursor itself simply never calls the compositor (or reports
 * `should_composite() == false`), which is why this is a seam rather
 * than a boolean.
 */
class NIMRTC_API_STABLE ICursorCompositor {
public:
    virtual ~ICursorCompositor() = default;

    /**
     * Whether this compositor should draw into the frame at all.
     *
     * False for a backend whose frames already carry the pointer.
     */
    [[nodiscard]] virtual bool should_composite() const noexcept = 0;

    /**
     * Draw the pointer into @p frame.
     *
     * @param frame    The BGRA pixels, in place.  `stride` is the row
     *                 stride in bytes; `width` / `height` are the
     *                 frame's dimensions.
     * @param state    The pointer's state.
     * @param origin_x The frame's x origin in *source* pixels (0 unless
     *                 a region was selected).
     * @param origin_y The frame's y origin in source pixels.
     *
     * @return true iff the frame was modified.
     *
     * A frame that is not BGRA, a hidden pointer, or a pointer that
     * falls entirely outside the frame all return false and leave the
     * frame untouched.
     */
    virtual bool composite(std::uint8_t* frame,
                           std::size_t frame_size,
                           std::int32_t stride,
                           std::uint32_t width,
                           std::uint32_t height,
                           const CursorState& state,
                           std::int32_t origin_x,
                           std::int32_t origin_y) noexcept = 0;

    /** A short identifier for the compositor, e.g. "alpha-blend",
     *  "pinned". */
    [[nodiscard]] virtual std::string_view backend() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// The portable implementation
// ---------------------------------------------------------------------------

/**
 * @brief The portable cursor compositor — an alpha-blended draw.
 *
 * This is the implementation every platform backend uses when the
 * platform does *not* composite the pointer itself.  It is portable
 * because it only touches the pixels: the pointer bitmap arrives
 * already in BGRA (the Windows and X11 cursor images are both available
 * in that form), so there is nothing platform-specific left.
 *
 * It also implements the **pinned** fallback: when the source cannot
 * report a live pointer, the last known position is kept and the
 * pointer stays on screen at that spot instead of vanishing.
 */
class NIMRTC_API_STABLE AlphaCursorCompositor final
    : public ICursorCompositor {
public:
    /**
     * @param pinned  When true, a source that cannot report a live
     *                pointer keeps drawing the last known one at its
     *                last known position.  This is the plan's
     *                "PinnedSink-style fallback".
     */
    explicit AlphaCursorCompositor(bool pinned = true) noexcept
        : pinned_(pinned) {}

    [[nodiscard]] bool should_composite() const noexcept override {
        return true;
    }

    bool composite(std::uint8_t* frame,
                   std::size_t frame_size,
                   std::int32_t stride,
                   std::uint32_t width,
                   std::uint32_t height,
                   const CursorState& state,
                   std::int32_t origin_x,
                   std::int32_t origin_y) noexcept override;

    [[nodiscard]] std::string_view backend() const noexcept override {
        return pinned_ ? "alpha-blend-pinned" : "alpha-blend";
    }

    /** True iff the pinned fallback is enabled. */
    [[nodiscard]] bool pinned() const noexcept { return pinned_; }

    /** How many frames this compositor has modified. */
    [[nodiscard]] std::uint64_t frames_composited() const noexcept {
        return frames_composited_;
    }

private:
    bool pinned_ = true;
    std::uint64_t frames_composited_ = 0;
};

// ---------------------------------------------------------------------------
// A source that never reports a pointer
// ---------------------------------------------------------------------------

/**
 * @brief A cursor source that reports "no pointer".
 *
 * This is the default for a platform that composites the cursor itself,
 * and the default for a build with no pointer support at all.  It exists
 * so the "no cursor" case is a *value* rather than a null pointer — the
 * compositor can be handed it without a null check at every call site.
 */
class NIMRTC_API_STABLE NullCursorSource final : public ICursorSource {
public:
    CursorState query() noexcept override { return CursorState{}; }

    [[nodiscard]] std::string_view backend() const noexcept override {
        return "none";
    }

    [[nodiscard]] bool has_live_cursor() const noexcept override {
        return false;
    }
};

// ---------------------------------------------------------------------------
// A source the host drives
// ---------------------------------------------------------------------------

/**
 * @brief A cursor source the host fills in.
 *
 * A host that already tracks the pointer (an embedded remote-desktop
 * agent that knows where the *remote* pointer is, a test that wants a
 * deterministic picture) can set the state directly instead of
 * implementing the platform queries.  The state is copied on the way
 * in, so the caller may reuse its buffer.
 */
class NIMRTC_API_STABLE ManualCursorSource final : public ICursorSource {
public:
    /** Replace the reported state. */
    void set_state(CursorState state) noexcept {
        state_ = std::move(state);
    }

    CursorState query() noexcept override { return state_; }

    [[nodiscard]] std::string_view backend() const noexcept override {
        return "manual";
    }

    [[nodiscard]] bool has_live_cursor() const noexcept override {
        return true;
    }

private:
    CursorState state_{};
};

// ---------------------------------------------------------------------------
// The pinned fallback
// ---------------------------------------------------------------------------

/**
 * @brief Holds the last known pointer state so it can be re-drawn.
 *
 * The plan's A6 row asks for "a `PinnedSink`-style 'cursor is always
 * drawn at the last position' fallback" for the case where the platform
 * cursor is unavailable.
 *
 * The holder is the piece that remembers the last position: it queries
 * the source, and when the source cannot report a live pointer it
 * returns the remembered state instead.  It is *not* a compositor — the
 * compositor still does the drawing.
 */
class NIMRTC_API_STABLE PinnedCursorSource final : public ICursorSource {
public:
    /**
     * @param inner The live source to query.  Not owned.
     */
    explicit PinnedCursorSource(ICursorSource* inner) noexcept
        : inner_(inner) {}

    CursorState query() noexcept override;

    [[nodiscard]] std::string_view backend() const noexcept override {
        return inner_ != nullptr ? inner_->backend() : "none";
    }

    [[nodiscard]] bool has_live_cursor() const noexcept override {
        return inner_ != nullptr && inner_->has_live_cursor();
    }

private:
    ICursorSource* inner_ = nullptr;
    CursorState pinned_{};
};

} // namespace nimrtc::video_capture
