// modules/video_capture/src/cursor_source.cpp
// ----------------------------------------------------------------------------
// The cursor *source* seam (remote-desktop plan A6) -- the portable
// half: the fallback and the platform dispatch.
//
// The platform halves live in `src/platform/windows/windows_cursor.cpp`
// and `src/platform/linux/x11_cursor.cpp`; this file holds everything
// that is platform-neutral.
//
// See include/nimrtc/video_capture/cursor_source.hpp for the contract.
// ----------------------------------------------------------------------------

#include <nimrtc/video_capture/cursor_source.hpp>

namespace nimrtc::video_capture {

// ---------------------------------------------------------------------------
// PinnedCursorFallback
// ---------------------------------------------------------------------------

PinnedCursorFallback::PinnedCursorFallback(std::shared_ptr<ICursorSource> inner) noexcept
    : inner_(std::move(inner)) {}

PinnedCursorFallback::~PinnedCursorFallback() = default;

CursorSourceStatus PinnedCursorFallback::poll(CursorSample& out) noexcept {
    if (!inner_) {
        // No source at all: there is nothing to pin and nothing to
        // serve.  The caller falls back to "no cursor in the frame".
        return CursorSourceStatus::kUnsupported;
    }

    CursorSample fresh;
    const CursorSourceStatus status = inner_->poll(fresh);

    if (status == CursorSourceStatus::kOk) {
        // Remember it, and serve it.
        pinned_sample_ = fresh;
        pinned_ = true;
        out = fresh;
        return CursorSourceStatus::kOk;
    }

    if (status == CursorSourceStatus::kNoOverlayNeeded ||
        status == CursorSourceStatus::kUnsupported) {
        // These are *not* transient: the platform composites the cursor
        // itself, or there is no cursor at all.  Pinning an old sample
        // and drawing it would be wrong -- it would put a cursor into a
        // picture that already has one.
        return status;
    }

    // `kUnavailable`: the platform could not be read right now.  Serve
    // the pinned sample -- the "always drawn at the last position"
    // behaviour the plan asks for.
    if (pinned_) {
        out = pinned_sample_;
        ++pinned_hits_;
        return CursorSourceStatus::kOk;
    }

    // Nothing has ever been pinned: there is no honest sample to serve.
    return CursorSourceStatus::kUnavailable;
}

void PinnedCursorFallback::clear_pin() noexcept {
    pinned_ = false;
    pinned_hits_ = 0;
    pinned_sample_ = CursorSample{};
}

// ---------------------------------------------------------------------------
// The platform dispatch
// ---------------------------------------------------------------------------

std::shared_ptr<ICursorSource> make_platform_cursor_source() noexcept {
#if defined(_WIN32)
    return make_windows_cursor_source();
#elif defined(__APPLE__)
    return make_macos_cursor_source();
#elif defined(NIMRTC_HAS_X11_CAPTURE)
    // The X11 source is only compiled when the X11 development headers
    // were found at configure time (the CMake guard), so the dispatch
    // must use the same condition -- otherwise a Linux box without the
    // X11 package would fail to link.
    return make_x11_cursor_source();
#else
    return nullptr;
#endif
}

bool platform_cursor_source_available() noexcept {
#if defined(_WIN32)
    return true;
#elif defined(__APPLE__)
    return true;
#elif defined(NIMRTC_HAS_X11_CAPTURE)
    // The X11 source needs the XFixes extension, which is present on
    // every X11 server since forever; the build guard in CMake decides
    // whether the implementation was compiled at all.
    return true;
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// The macOS source
// ---------------------------------------------------------------------------

namespace {

/**
 * @brief The macOS cursor source.
 *
 * ScreenCaptureKit composites the pointer into the picture itself, so
 * an overlay would draw the cursor *twice*.  The source therefore
 * reports `kNoOverlayNeeded` — which is a distinct status from
 * `kUnsupported`, because the platform *does* have a cursor, the
 * capture path just does not need it.
 */
class MacosCursorSource final : public ICursorSource {
public:
    [[nodiscard]] CursorSourceStatus poll(CursorSample&) noexcept override {
        return CursorSourceStatus::kNoOverlayNeeded;
    }

    [[nodiscard]] std::string_view id() const noexcept override {
        return "macos-cursor";
    }
};

} // anonymous namespace

std::shared_ptr<ICursorSource> make_macos_cursor_source() noexcept {
#if defined(__APPLE__)
    return std::make_shared<MacosCursorSource>();
#else
    // The class exists on every platform so the header's contract is
    // uniform, but a non-macOS build does not pretend to have one.
    return nullptr;
#endif
}

} // namespace nimrtc::video_capture
